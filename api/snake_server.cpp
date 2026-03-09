// snake_server.cpp
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <limits>
#include <optional>
#include <random>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <aws/core/Aws.h>
#include <aws/core/client/ClientConfiguration.h>
#include <aws/core/http/HttpClientFactory.h>
#include <aws/core/http/HttpClient.h>
#include <aws/core/http/HttpRequest.h>
#include <aws/core/http/HttpTypes.h>
#include <aws/core/utils/StringUtils.h>

#include "economy/economy_v1.h"
#include "economy/stabilization_engine.h"
#include "economy_engine/compute.h"
#include "httplib.h"
#include "persistence/coordinator/persistence_coordinator.h"
#include "persistence/layers/dynamo/permanent_dynamo_store.h"
#include "persistence/layers/runtime/runtime_state_store.h"
#include "persistence/layers/sqlite/buffered_sqlite_store.h"
#include "persistence/profiles/persistence_profiles.h"
#include "persistence/router/persistence_router.h"
#include "protocol/encode_json.h"
#include "storage/storage_factory.h"
#include "world/world.h"
#include "../config/runtime_config.h"

using namespace std;

static constexpr int DEFAULT_W = 40;
static constexpr int DEFAULT_H = 20;
static constexpr int DEFAULT_FOOD_COUNT = 1;

static volatile sig_atomic_t g_reload_requested = 0;

static void on_reload_signal(int) {
  g_reload_requested = 1;
}

static uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static pair<int, int> dims_from_area(int64_t area, double aspect_ratio) {
  const int64_t safe_area = max<int64_t>(100, area);
  const double safe_aspect = (aspect_ratio > 0.1) ? aspect_ratio : 16.0 / 9.0;
  int width = max(10, static_cast<int>(ceil(sqrt(static_cast<double>(safe_area) * safe_aspect))));
  int height = max(10, static_cast<int>(ceil(static_cast<double>(safe_area) / static_cast<double>(width))));
  while (static_cast<int64_t>(width) * static_cast<int64_t>(height) < safe_area) {
    ++height;
  }
  return {width, height};
}

static int64_t economy_world_area(const storage::EconomyParams& params,
                                  const economy::EconomySnapshot& global) {
  const int64_t k_land = std::max<int64_t>(1, params.k_land);
  return std::max<int64_t>(100, k_land * global.m);
}

struct EconomyActivityDelta {
  int64_t harvested_food = 0;
  int64_t movement_ticks = 0;
  std::unordered_map<int, int64_t> harvested_food_by_user;
  std::unordered_map<int, int64_t> movement_ticks_by_user;

  bool empty() const {
    return harvested_food == 0 && movement_ticks == 0 && harvested_food_by_user.empty() &&
           movement_ticks_by_user.empty();
  }
};

static string json_escape(const string& s) {
  ostringstream o;
  for (char c : s) {
    switch (c) {
      case '"':
        o << "\\\"";
        break;
      case '\\':
        o << "\\\\";
        break;
      case '\b':
        o << "\\b";
        break;
      case '\f':
        o << "\\f";
        break;
      case '\n':
        o << "\\n";
        break;
      case '\r':
        o << "\\r";
        break;
      case '\t':
        o << "\\t";
        break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          o << "\\u" << hex << setw(4) << setfill('0') << static_cast<int>(c);
        } else {
          o << c;
        }
    }
  }
  return o.str();
}

static string json_number(double v) {
  if (!std::isfinite(v)) return "0";
  ostringstream out;
  out << fixed << setprecision(6) << v;
  string s = out.str();
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.push_back('0');
  if (s.empty()) s = "0";
  return s;
}

static std::string stabilization_status_ui(const economy::StabilizationRuntimeState& st) {
  if (st.liquidity_constraint_mode_active) return "Liquidity Tightening";
  if (st.expansion_recent_checks_remaining > 0) return "Expanding";
  return "Stable";
}

static string rand_token(size_t n = 32) {
  static const char* chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static thread_local mt19937 rng(static_cast<uint32_t>(random_device{}()));
  uniform_int_distribution<int> dist(0, static_cast<int>(strlen(chars)) - 1);
  string t;
  t.reserve(n);
  for (size_t i = 0; i < n; ++i) t.push_back(chars[dist(rng)]);
  return t;
}

static bool is_strict_semver(const std::string& v) {
  static const std::regex re(R"(^\d+\.\d+\.\d+$)");
  return std::regex_match(v, re);
}

class AuthState {
 public:
  optional<int> token_to_user(const string& token) {
    lock_guard<mutex> lock(mu_);
    auto it = token_to_uid_.find(token);
    if (it == token_to_uid_.end()) return nullopt;
    return it->second;
  }

  string issue_token(int user_id) {
    lock_guard<mutex> lock(mu_);
    string token = rand_token();
    token_to_uid_[token] = user_id;
    return token;
  }

 private:
  mutex mu_;
  unordered_map<string, int> token_to_uid_;
};

struct ClientSession {
  string session_id;
  int camera_x = DEFAULT_W / 2;
  int camera_y = DEFAULT_H / 2;
  double camera_zoom = 1.0;
  int subscribed_chunks_count = 1;
  optional<int> auth_user_id;
  optional<int> watched_snake_id;
  bool is_watcher = true;
  uint64_t last_camera_update_ms = 0;
  uint64_t last_system_message_id = 0;
  uint64_t updated_at_ms = 0;
};

struct PublicViewState {
  int camera_x = DEFAULT_W / 2;
  int camera_y = DEFAULT_H / 2;
  int chunk_cx = 0;
  int chunk_cy = 0;
  uint64_t last_switch_tick = 0;
  bool initialized = false;
};

struct SystemMessage {
  uint64_t id = 0;
  std::string level = "info";
  std::string text;
  int64_t created_at = 0;
};

class SystemMessageBus {
 public:
  uint64_t Publish(const std::string& level, const std::string& text) {
    std::lock_guard<std::mutex> lock(mu_);
    SystemMessage msg;
    msg.id = ++next_id_;
    msg.level = level.empty() ? "info" : level;
    msg.text = text;
    msg.created_at = static_cast<int64_t>(time(nullptr));
    messages_.push_back(std::move(msg));
    if (messages_.size() > 64) {
      messages_.erase(messages_.begin(), messages_.begin() + static_cast<long>(messages_.size() - 64));
    }
    return next_id_;
  }

  std::vector<SystemMessage> GetSince(uint64_t last_seen_id) const {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<SystemMessage> out;
    for (const auto& m : messages_) {
      if (m.id > last_seen_id) out.push_back(m);
    }
    return out;
  }

 private:
  mutable std::mutex mu_;
  uint64_t next_id_ = 0;
  std::vector<SystemMessage> messages_;
};

class GameService {
 public:
  GameService(storage::IStorage& storage,
              persistence::IPersistenceCoordinator& persistence_coordinator,
              int width,
              int height,
              int food_count,
              int max_snakes_per_user)
      : storage_(storage),
        persistence_coordinator_(persistence_coordinator),
        world_(width, height, food_count, max_snakes_per_user) {}

  void configure_chunking(int chunk_size, bool single_chunk_mode) {
    world_.ConfigureChunking(chunk_size, single_chunk_mode);
  }

  void configure_mask(const string& mode, int seed, const string& style) {
    world_.ConfigureMask(mode, seed, style);
  }

  void set_playable_cell_target(int64_t playable_cells_target) {
    world_.SetPlayableCellTarget(playable_cells_target);
  }

  void load_from_storage_or_seed_positions() {
    world_.LoadFromStorage(storage_.ListSnakes(), storage_.GetWorldChunk("main"));
  }

  void tick() {
    world_.Tick();
  }

  world::WorldSnapshot snapshot() {
    ensure_loaded_from_storage_if_empty();
    return world_.Snapshot();
  }

  world::WorldSnapshot snapshot_for_camera(int camera_x,
                                           int camera_y,
                                           bool aoi_enabled,
                                           int aoi_radius,
                                           bool debug_validate_bounds = false) {
    ensure_loaded_from_storage_if_empty();
    return world_.SnapshotForCamera(camera_x, camera_y, aoi_enabled, aoi_radius, aoi_pad_chunks_, debug_validate_bounds);
  }

  world::ChunkId coord_to_chunk(int x, int y) {
    ensure_loaded_from_storage_if_empty();
    return world_.CoordToChunk(x, y);
  }

  world::Vec2 chunk_center_to_world(const world::ChunkId& id) {
    ensure_loaded_from_storage_if_empty();
    return world_.ChunkCenterToWorld(id);
  }

  void set_aoi_pad_chunks(int pad) {
    aoi_pad_chunks_ = max(0, pad);
  }

  void set_duel_delay_ticks(int ticks) {
    world_.SetDuelDelayTicks(std::max(1, ticks));
  }

  bool set_snake_dir(int user_id, int snake_id, world::Dir d) {
    return world_.QueueDirectionInput(user_id, snake_id, d);
  }

  bool toggle_snake_pause(int user_id, int snake_id) {
    return world_.QueuePauseToggle(user_id, snake_id);
  }

  vector<world::Snake> list_user_snakes(int user_id) {
    ensure_loaded_from_storage_if_empty();
    return world_.ListUserSnakes(user_id);
  }

  optional<int> create_snake_for_user(int user_id, const string& color) {
    return world_.CreateSnakeForUser(user_id, color);
  }

  optional<int> attach_cells_to_snake(int user_id, int snake_id, int amount) {
    ensure_loaded_from_storage_if_empty();
    return world_.AttachCellsForUser(user_id, snake_id, amount);
  }

  void resize_world(int width, int height) {
    world_.ResizeWorld(width, height);
  }

  // Expands playable space via the existing resize/mask pipeline at a safe call site.
  int64_t expand_playable_cells(int64_t cells_to_add, double aspect_ratio) {
    if (cells_to_add <= 0) return 0;
    const auto before = world_.Snapshot();
    const int64_t target_playable = std::max<int64_t>(0, before.playable_cells) + cells_to_add;
    const int64_t current_area = static_cast<int64_t>(before.w) * static_cast<int64_t>(before.h);
    if (target_playable > current_area) {
      const auto dims = dims_from_area(target_playable, aspect_ratio);
      world_.ResizeWorld(dims.first, dims.second);
    }
    world_.SetPlayableCellTarget(target_playable);
    const auto after = world_.Snapshot();
    return std::max<int64_t>(0, after.playable_cells - before.playable_cells);
  }

  // Writes only event-driven deltas. No per-tick checkpoint persistence.
  EconomyActivityDelta flush_persistence_delta_and_credit_food(int64_t food_reward_cells) {
    EconomyActivityDelta out_activity;
    const auto delta = world_.DrainPersistenceDelta(static_cast<int64_t>(now_ms()));
    if (delta.empty()) return out_activity;
    out_activity.harvested_food = delta.harvested_food;
    out_activity.movement_ticks = delta.movement_ticks;
    out_activity.harvested_food_by_user = delta.harvested_food_by_user;
    out_activity.movement_ticks_by_user = delta.movement_ticks_by_user;

    std::unordered_map<std::string, std::string> owner_by_snake_id;
    owner_by_snake_id.reserve(delta.upsert_snakes.size());
    for (const auto& s : delta.upsert_snakes) {
      if (!s.snake_id.empty() && !s.owner_user_id.empty()) {
        owner_by_snake_id[s.snake_id] = s.owner_user_id;
      }
    }

    std::unique_ptr<std::unordered_map<std::string, std::string>> snapshot_owner_map;
    auto resolve_owner_user_id = [&](const std::string& snake_id) -> std::string {
      auto it = owner_by_snake_id.find(snake_id);
      if (it != owner_by_snake_id.end()) return it->second;
      if (!snapshot_owner_map) {
        snapshot_owner_map = std::make_unique<std::unordered_map<std::string, std::string>>();
        const auto snap = world_.Snapshot();
        snapshot_owner_map->reserve(snap.snakes.size());
        for (const auto& ws : snap.snakes) {
          snapshot_owner_map->emplace(std::to_string(ws.id), std::to_string(ws.user_id));
        }
      }
      auto sit = snapshot_owner_map->find(snake_id);
      if (sit != snapshot_owner_map->end()) return sit->second;
      return "";
    };

    int64_t credited_food_events = 0;
    if (food_reward_cells > 0) {
      for (const auto& e : delta.snake_events) {
        if (e.event_type != "FOOD_EATEN") continue;
        const std::string owner_user_id = resolve_owner_user_id(e.snake_id);
        if (owner_user_id.empty()) continue;
        persistence::PersistenceIntent intent;
        intent.type = persistence::IntentType::UserBalanceChanged;
        intent.user_id = owner_user_id;
        intent.delta_i64 = food_reward_cells;
        if (persistence_coordinator_.Emit(intent)) {
          ++credited_food_events;
        }
      }
    }
    for (const auto& ud : delta.user_balance_deltas) {
      if (ud.first.empty() || ud.second == 0) continue;
      persistence::PersistenceIntent intent;
      intent.type = persistence::IntentType::UserBalanceChanged;
      intent.user_id = ud.first;
      intent.delta_i64 = ud.second;
      (void)persistence_coordinator_.Emit(intent);
    }
    if (delta.system_balance_delta != 0) {
      persistence::PersistenceIntent intent;
      intent.type = persistence::IntentType::SnakeDeathSettled;
      intent.delta_i64 = delta.system_balance_delta;
      (void)persistence_coordinator_.Emit(intent);
    }

    for (const auto& s : delta.upsert_snakes) {
      persistence::PersistenceIntent intent;
      intent.type = persistence::IntentType::SnakeSnapshotUpdated;
      intent.snake_snapshot = s;
      (void)persistence_coordinator_.Emit(intent);
    }
    for (const auto& sid : delta.delete_snake_ids) {
      persistence::PersistenceIntent intent;
      intent.type = persistence::IntentType::SnakeSnapshotDeleted;
      storage::Snake s;
      s.snake_id = sid;
      intent.snake_snapshot = s;
      (void)persistence_coordinator_.Emit(intent);
    }
    if (delta.upsert_world_chunk.has_value()) {
      persistence::PersistenceIntent intent;
      intent.type = persistence::IntentType::WorldChunkDirty;
      intent.world_chunk = *delta.upsert_world_chunk;
      (void)persistence_coordinator_.Emit(intent);
    }
    for (const auto& e : delta.snake_events) {
      persistence::PersistenceIntent intent;
      intent.type = persistence::IntentType::SnakeEventLogged;
      intent.snake_event = e;
      (void)persistence_coordinator_.Emit(intent);
    }
    (void)credited_food_events;
    return out_activity;
  }

  void flush_persistence_delta() {
    (void)flush_persistence_delta_and_credit_food(0);
  }


 private:
  void ensure_loaded_from_storage_if_empty() {
    const auto snap = world_.Snapshot();
    if (!snap.snakes.empty()) return;

    const auto now = chrono::steady_clock::now();
    if (now - last_empty_reload_attempt_ < chrono::seconds(2)) return;
    last_empty_reload_attempt_ = now;

    // Smartseed writes directly to DynamoDB; this keeps runtime world in sync
    // without requiring a process restart.
    load_from_storage_or_seed_positions();
  }

  storage::IStorage& storage_;
  persistence::IPersistenceCoordinator& persistence_coordinator_;
  world::World world_;
  int aoi_pad_chunks_ = 0;
  chrono::steady_clock::time_point last_empty_reload_attempt_{};
};

class EconomyService {
 public:
  struct StabilizationActions {
    std::function<int64_t(int64_t)> expand_playable_cells;
    std::function<void(const std::string&, const std::string&, const std::string&)> emit_system_message;
    std::function<world::WorldSnapshot()> current_world_snapshot;
  };

  explicit EconomyService(storage::IStorage& storage, const RuntimeConfig& runtime_cfg)
      : storage_(storage),
        period_cfg_{std::max(60, runtime_cfg.econ_period_seconds), runtime_cfg.econ_period_align},
        flush_interval_sec_(std::max(2, runtime_cfg.economy_flush_seconds)),
        stabilization_cfg_{runtime_cfg.auto_expansion_enabled,
                           runtime_cfg.auto_expansion_trigger_ratio,
                           runtime_cfg.target_spatial_ratio,
                           runtime_cfg.auto_expansion_checks_per_period,
                           runtime_cfg.target_lcr,
                           runtime_cfg.lcr_stress_threshold,
                           runtime_cfg.max_auto_money_growth},
        stabilization_engine_(stabilization_cfg_),
        fast_check_interval_ms_(std::max<int64_t>(
            1000,
            static_cast<int64_t>(std::llround(
                (1000.0 * static_cast<double>(std::max(60, runtime_cfg.econ_period_seconds))) /
                static_cast<double>(std::max(1, runtime_cfg.auto_expansion_checks_per_period)))))),
        cache_ttl_ms_([] {
          const char* env = getenv("ECONOMY_CACHE_MS");
          if (!env) return 2000;
          const int parsed = atoi(env);
          return max(500, min(10000, parsed));
        }()) {
    if (!runtime_cfg.econ_period_tz.empty()) {
#if !defined(_WIN32)
      setenv("TZ", runtime_cfg.econ_period_tz.c_str(), 1);
      tzset();
#endif
    }
    next_fast_check_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(fast_check_interval_ms_);
  }

  struct Snapshot {
    storage::EconomyParams params;
    economy::EconomySnapshot global;
    std::optional<economy::EconomyUserSnapshot> user;
    int64_t k_snakes = 0;
    std::string period_id;
    int64_t period_ends_in_seconds = 0;
    economy::StabilizationDerived stabilization;
    economy::StabilizationRuntimeState stabilization_runtime;
    int64_t next_fast_check_in_seconds = 0;
  };

  void SetStabilizationActions(StabilizationActions actions) {
    lock_guard<mutex> lock(mu_);
    stabilization_actions_ = std::move(actions);
  }

  void OnActivity(const EconomyActivityDelta& d) {
    if (d.empty()) {
      return;
    }
    lock_guard<mutex> lock(mu_);
    EnsurePeriodLocked();
    pending_harvested_food_ += d.harvested_food;
    pending_real_output_ += d.harvested_food;
    pending_movement_ticks_ += d.movement_ticks;
    for (const auto& kv : d.harvested_food_by_user) {
      if (kv.first <= 0 || kv.second == 0) continue;
      pending_user_harvested_[kv.first] += kv.second;
      pending_user_real_output_[kv.first] += kv.second;
    }
    for (const auto& kv : d.movement_ticks_by_user) {
      if (kv.first <= 0 || kv.second == 0) continue;
      pending_user_movement_[kv.first] += kv.second;
    }
    MaybeFlushPendingLocked(false);
    cache_valid_ = false;
  }

  void TickStabilization() {
    using clock = std::chrono::steady_clock;
    const auto now = clock::now();
    lock_guard<mutex> lock(mu_);
    EnsurePeriodLocked();
    if (now < next_fast_check_at_) return;
    MaybeFlushPendingLocked(false);
    next_fast_check_at_ = now + std::chrono::milliseconds(fast_check_interval_ms_);
    if (!stabilization_cfg_.auto_expansion_enabled) return;

    auto state = ComputeFresh(std::nullopt);
    const auto decision = stabilization_engine_.EvaluateFastSpatialCheck(state.stabilization);
    if (!decision.triggered) return;

    if (decision.should_expand && decision.required_expansion_cells > 0) {
      if (stabilization_actions_.expand_playable_cells) {
        const int64_t expanded_cells = stabilization_actions_.expand_playable_cells(decision.required_expansion_cells);
        if (expanded_cells > 0) {
          stabilization_engine_.OnSpatialExpansionApplied();
          cache_valid_ = false;
          if (stabilization_actions_.emit_system_message) {
            stabilization_actions_.emit_system_message(
                "spatial_expansion",
                "info",
                u8"🌍 World Expansion Event\nSpatial liquidity dropped below safe levels.\nThe Central Authority has expanded the world boundaries.");
          }
          storage::SnakeEvent event;
          event.snake_id = "system";
          event.event_id = std::to_string(static_cast<int64_t>(time(nullptr))) + "#spatial_expansion";
          event.event_type = "SYSTEM_SPATIAL_EXPANSION";
          event.delta_length = static_cast<int>(std::min<int64_t>(expanded_cells, static_cast<int64_t>(std::numeric_limits<int>::max())));
          event.created_at = static_cast<int64_t>(time(nullptr));
          (void)storage_.AppendSnakeEvent(event);
        }
      }
      return;
    }

    if (decision.entered_liquidity_constraint_mode && stabilization_actions_.emit_system_message) {
      stabilization_actions_.emit_system_message(
          "liquidity_constraint",
          "warning",
          u8"⚠ Economic Liquidity Tightening\nThe Central Authority has exhausted its spatial reserves.\nExpansion is temporarily paused until the next monetary cycle.");
      storage::SnakeEvent event;
      event.snake_id = "system";
      event.event_id = std::to_string(static_cast<int64_t>(time(nullptr))) + "#liquidity_constraint";
      event.event_type = "SYSTEM_LIQUIDITY_CONSTRAINT";
      event.created_at = static_cast<int64_t>(time(nullptr));
      (void)storage_.AppendSnakeEvent(event);
    }
  }

  Snapshot GetState(std::optional<int> user_id = std::nullopt) {
    using clock = chrono::steady_clock;
    const auto now = clock::now();

    {
      lock_guard<mutex> lock(mu_);
      EnsurePeriodLocked();
      if (cache_valid_ && now < cache_expire_at_ && cache_user_id_ == user_id) return cache_;
      MaybeFlushPendingLocked(false);
    }

    Snapshot fresh = ComputeFresh(user_id);
    {
      lock_guard<mutex> lock(mu_);
      cache_ = fresh;
      cache_user_id_ = user_id;
      cache_valid_ = true;
      cache_expire_at_ = now + chrono::milliseconds(cache_ttl_ms_);
      return cache_;
    }
  }

  Snapshot RecomputeAndPersist(const string& period_id,
                               std::optional<int> user_id = std::nullopt,
                               bool force_rewrite = false) {
    lock_guard<mutex> lock(mu_);
    EnsurePeriodLocked();
    MaybeFlushPendingLocked(true);
    if (!FinalizePeriodLocked(period_id, force_rewrite)) {
      // Keep cached values intact if recompute is rejected.
      return ComputeFresh(user_id);
    }
    cache_valid_ = false;
    return ComputeFresh(user_id);
  }

  struct DebugState {
    std::string period_id;
    int64_t period_ends_in_seconds = 0;
    int64_t pending_harvested_food = 0;
    int64_t pending_real_output = 0;
    int64_t pending_movement_ticks = 0;
    size_t pending_users = 0;
    int flush_interval_sec = 10;
    int64_t seconds_since_last_flush = 0;
  };

  DebugState GetDebugState() {
    lock_guard<mutex> lock(mu_);
    EnsurePeriodLocked();
    using namespace std::chrono;
    const auto now = steady_clock::now();
    DebugState out;
    out.period_id = current_period_id_;
    out.period_ends_in_seconds = current_period_ends_in_seconds_;
    out.pending_harvested_food = pending_harvested_food_;
    out.pending_real_output = pending_real_output_;
    out.pending_movement_ticks = pending_movement_ticks_;
    out.pending_users = pending_user_harvested_.size() + pending_user_real_output_.size() + pending_user_movement_.size();
    out.flush_interval_sec = flush_interval_sec_;
    out.seconds_since_last_flush = static_cast<int64_t>(
        duration_cast<seconds>(now - last_flush_at_).count());
    return out;
  }

  void InvalidateCache() {
    lock_guard<mutex> lock(mu_);
    cache_valid_ = false;
  }

 private:
  // Canonical derived economy/spatial view used by WS/HTTP/admin and stabilization logic.
  economy::StabilizationDerived BuildCanonicalSpatialDerived(const storage::EconomyParams& params,
                                                             int64_t money_supply,
                                                             int64_t deployed_capital,
                                                             const std::optional<world::WorldSnapshot>& world_snap) {
    const int64_t occupied_cells = world_snap.has_value()
                                       ? economy::StabilizationEngine::ComputeOccupiedSnakeCells(*world_snap)
                                       : std::max<int64_t>(0, deployed_capital);
    const int64_t field_size = world_snap.has_value()
                                   ? std::max<int64_t>(0, world_snap->playable_cells)
                                   : economy_world_area(params, economy::EconomySnapshot{});
    const int64_t free_space_on_field = std::max<int64_t>(0, field_size - occupied_cells);
    return stabilization_engine_.Derive(money_supply, params.k_land, deployed_capital, field_size, free_space_on_field);
  }

  void EnsurePeriodLocked() {
    const auto ps = economy::CurrentPeriodState(std::time(nullptr), period_cfg_);
    if (current_period_id_.empty()) {
      current_period_id_ = ps.period_id;
      current_period_ends_in_seconds_ = ps.ends_in_seconds;
      return;
    }
    current_period_ends_in_seconds_ = ps.ends_in_seconds;
    if (ps.period_id == current_period_id_) return;

    MaybeFlushPendingLocked(true);
    (void)FinalizePeriodLocked(current_period_id_, false);
    current_period_id_ = ps.period_id;
    pending_harvested_food_ = 0;
    pending_real_output_ = 0;
    pending_movement_ticks_ = 0;
    pending_user_harvested_.clear();
    pending_user_real_output_.clear();
    pending_user_movement_.clear();
    stabilization_engine_.ResetForNewPeriod();
    next_fast_check_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(fast_check_interval_ms_);
  }

  void MaybeFlushPendingLocked(bool force) {
    if (current_period_id_.empty()) return;
    const auto now = chrono::steady_clock::now();
    if (!force && (now - last_flush_at_) < chrono::seconds(flush_interval_sec_)) {
      return;
    }
    if (pending_harvested_food_ != 0 || pending_movement_ticks_ != 0) {
      (void)storage_.IncrementEconomyPeriodRaw(current_period_id_, pending_harvested_food_, pending_movement_ticks_);
      pending_harvested_food_ = 0;
      pending_real_output_ = 0;
      pending_movement_ticks_ = 0;
    }
    std::unordered_set<int> users;
    for (const auto& kv : pending_user_harvested_) users.insert(kv.first);
    for (const auto& kv : pending_user_real_output_) users.insert(kv.first);
    for (const auto& kv : pending_user_movement_) users.insert(kv.first);
    for (int uid : users) {
      const int64_t h = pending_user_harvested_[uid];
      (void)pending_user_real_output_[uid];
      const int64_t m = pending_user_movement_[uid];
      if (h == 0 && m == 0) continue;
      (void)storage_.IncrementEconomyPeriodUserRaw(current_period_id_, std::to_string(uid), h, m);
    }
    pending_user_harvested_.clear();
    pending_user_real_output_.clear();
    pending_user_movement_.clear();
    last_flush_at_ = now;
  }

  bool FinalizePeriodLocked(const std::string& period_id, bool force_rewrite) {
    if (period_id.empty()) return false;
    auto period = storage_.GetEconomyPeriod(period_id).value_or(storage::EconomyPeriod{});
    period.period_key = period_id;
    if (period.is_finalized && !force_rewrite) {
      return false;
    }
    const auto params = storage_.GetEconomyParamsActive().value_or(storage::EconomyParams{});
    const auto users = storage_.ListUsers();
    const auto snakes = storage_.ListSnakes();

    int64_t sum_mi = 0;
    for (const auto& u : users) sum_mi += u.balance_mi;
    const auto capital = economy_engine::AggregateProductiveCapital(snakes);
    world::WorldSnapshot world_snap;
    bool has_world_snapshot = false;
    if (stabilization_actions_.current_world_snapshot) {
      world_snap = stabilization_actions_.current_world_snapshot();
      has_world_snapshot = true;
    }
    const auto derived_close_base = BuildCanonicalSpatialDerived(
        params, sum_mi + params.m_gov_reserve, capital.total_capital, has_world_snapshot ? std::optional<world::WorldSnapshot>(world_snap) : std::nullopt);
    const auto& derived_close = derived_close_base;
    const auto close_decision = stabilization_engine_.EvaluatePeriodClose(period_id, derived_close, sum_mi + params.m_gov_reserve);
    std::optional<storage::EconomyParams> stabilized_params;
    if (!close_decision.already_handled_for_period) {
      if (close_decision.should_expand_money && close_decision.actual_money_expansion > 0) {
        stabilized_params = params;
        stabilized_params->m_gov_reserve += close_decision.actual_money_expansion;
        stabilized_params->updated_at = static_cast<int64_t>(time(nullptr));
        stabilized_params->updated_by = "stabilization_engine";
        (void)storage_.PutEconomyParamsActiveAndVersioned(*stabilized_params, stabilized_params->updated_by);
        if (stabilization_actions_.emit_system_message) {
          stabilization_actions_.emit_system_message(
              "monetary_expansion",
              "info",
              u8"💰 Monetary Adjustment\nReserve coverage fell below stability threshold.\nThe monetary base has been expanded to restore liquidity.");
        }
        storage::SnakeEvent event;
        event.snake_id = "system";
        event.event_id = std::to_string(static_cast<int64_t>(time(nullptr))) + "#monetary_expansion";
        event.event_type = "SYSTEM_MONETARY_EXPANSION";
        event.delta_length = static_cast<int>(
            std::min<int64_t>(close_decision.actual_money_expansion, static_cast<int64_t>(std::numeric_limits<int>::max())));
        event.created_at = static_cast<int64_t>(time(nullptr));
        (void)storage_.AppendSnakeEvent(event);
      } else if (close_decision.should_emit_no_adjustment) {
        if (stabilization_actions_.emit_system_message) {
          stabilization_actions_.emit_system_message(
              "monetary_stable",
              "info",
              u8"🏛 Monetary Base Stable\nReserve levels remain sufficient. No adjustment required.");
        }
        storage::SnakeEvent event;
        event.snake_id = "system";
        event.event_id = std::to_string(static_cast<int64_t>(time(nullptr))) + "#monetary_stable";
        event.event_type = "SYSTEM_MONETARY_STABLE";
        event.created_at = static_cast<int64_t>(time(nullptr));
        (void)storage_.AppendSnakeEvent(event);
      }
    }
    const auto active_params_after_stabilization =
        stabilized_params.has_value() ? *stabilized_params : params;

    economy::EconomyPeriodRaw raw;
    raw.harvested_food = period.harvested_food;
    raw.real_output = period.real_output > 0 ? period.real_output : period.harvested_food;
    raw.movement_ticks = period.movement_ticks;
    raw.deployed_cells = capital.total_capital + active_params_after_stabilization.delta_k_obs;
    raw.alpha_bootstrap_default = active_params_after_stabilization.alpha_bootstrap_default;
    auto global =
        economy_engine::ComputeGlobal(raw, last_closed_global_, sum_mi, active_params_after_stabilization.m_gov_reserve);
    global.period_id = period_id;
    global.snapshot_status = "cached";

    period.total_output = global.y;
    period.real_output = raw.real_output;
    period.total_capital = global.k;
    period.total_labor = global.l;
    period.capital_share = global.alpha;
    period.productivity_index = global.a;
    period.money_supply = global.m;
    period.price_index = global.p;
    period.inflation_rate = global.pi;
    period.price_index_valid = global.price_index_valid;
    period.inflation_valid = global.inflation_valid;
    period.treasury_balance = global.treasury_balance;
    period.alpha_bootstrap = global.alpha_bootstrap;
    period.snapshot_status = "cached";
    period.is_finalized = true;
    period.finalized_at = static_cast<int64_t>(time(nullptr));
    period.computed_at = static_cast<int64_t>(time(nullptr));
    period.computed_m = period.money_supply;
    period.computed_k = period.total_capital;
    period.computed_y = period.total_output;
    period.computed_p = static_cast<int64_t>(period.price_index * 1000000.0);
    period.computed_pi = static_cast<int64_t>(period.inflation_rate * 1000000.0);
    period.computed_world_area = global.m;  // legacy unused by v1 core
    period.computed_white = 0;
    const int64_t expected_m = sum_mi + active_params_after_stabilization.m_gov_reserve;
    if (period.money_supply != expected_m) {
      static int64_t last_warn_ts = 0;
      const int64_t now_ts = static_cast<int64_t>(time(nullptr));
      if (now_ts - last_warn_ts >= 60) {
        std::cerr << "[economy] money supply mismatch period=" << period_id
                  << " computed=" << period.money_supply
                  << " expected=" << expected_m << "\n";
        last_warn_ts = now_ts;
      }
    }
    (void)storage_.PutEconomyPeriod(period);

    auto user_raw_rows = storage_.ListEconomyPeriodUsers(period_id);
    std::unordered_map<std::string, storage::EconomyPeriodUser> user_rows;
    for (const auto& row : user_raw_rows) user_rows[row.user_id] = row;

    for (const auto& u : users) {
      auto row = user_rows.count(u.user_id) ? user_rows[u.user_id] : storage::EconomyPeriodUser{};
      row.period_key = period_id;
      row.user_id = u.user_id;
      economy::EconomyPeriodRaw user_raw;
      user_raw.harvested_food = row.user_harvested_food;
      user_raw.real_output = row.user_real_output > 0 ? row.user_real_output : row.user_harvested_food;
      user_raw.movement_ticks = row.user_movement_ticks;
      if (const auto it_cap = capital.user_total_capital.find(u.user_id);
          it_cap != capital.user_total_capital.end()) {
        user_raw.deployed_cells = it_cap->second;
      } else {
        user_raw.deployed_cells = 0;
      }
      user_raw.alpha_bootstrap_default = active_params_after_stabilization.alpha_bootstrap_default;
      std::optional<economy::EconomyUserSnapshot> prev_user;
      auto it_prev = last_closed_users_.find(u.user_id);
      if (it_prev != last_closed_users_.end()) prev_user = it_prev->second;
      auto us = economy_engine::ComputeUser(user_raw, prev_user, u.balance_mi, global.y, period_id, u.user_id);
      row.user_output = us.y_u;
      row.user_real_output = user_raw.real_output;
      row.user_capital = us.k_u;
      row.user_labor = us.l_u;
      row.user_capital_share = us.alpha_u;
      row.user_productivity = us.a_u;
      row.user_market_share = us.market_share;
      row.user_storage_balance = us.storage_balance;
      row.alpha_bootstrap = us.alpha_bootstrap;
      row.computed_at = period.computed_at;
      (void)storage_.PutEconomyPeriodUser(row);
      last_closed_users_[u.user_id] = us;
    }
    last_closed_global_ = global;
    return true;
  }

  Snapshot ComputeFresh(std::optional<int> user_id) {
    Snapshot out;
    out.params = storage_.GetEconomyParamsActive().value_or(storage::EconomyParams{});
    out.period_id = current_period_id_;
    out.period_ends_in_seconds = current_period_ends_in_seconds_;
    auto period = storage_.GetEconomyPeriod(current_period_id_).value_or(storage::EconomyPeriod{});
    period.period_key = current_period_id_;

    const auto users = storage_.ListUsers();
    int64_t sum_mi = 0;
    for (const auto& u : users) sum_mi += u.balance_mi;

    const auto snakes = storage_.ListSnakes();
    const auto capital = economy_engine::AggregateProductiveCapital(snakes);
    out.k_snakes = capital.total_capital;

    economy::EconomyPeriodRaw raw;
    raw.harvested_food = period.harvested_food;
    raw.real_output = period.real_output > 0 ? period.real_output : period.harvested_food;
    raw.movement_ticks = period.movement_ticks;
    raw.deployed_cells = out.k_snakes + out.params.delta_k_obs;
    raw.alpha_bootstrap_default = out.params.alpha_bootstrap_default;
    out.global = economy_engine::ComputeGlobal(raw, last_closed_global_, sum_mi, out.params.m_gov_reserve);
    out.global.period_id = current_period_id_;
    out.global.period_ends_in_seconds = current_period_ends_in_seconds_;
    out.global.snapshot_status = period.is_finalized ? "cached" : "live_unfinalized";

    if (user_id.has_value()) {
      const std::string uid = std::to_string(*user_id);
      auto user = storage_.GetUserById(uid);
      if (user.has_value()) {
        auto row = storage_.GetEconomyPeriodUser(current_period_id_, uid).value_or(storage::EconomyPeriodUser{});
        row.period_key = current_period_id_;
        row.user_id = uid;
        economy::EconomyPeriodRaw uraw;
        uraw.harvested_food = row.user_harvested_food;
        uraw.real_output = row.user_real_output > 0 ? row.user_real_output : row.user_harvested_food;
        uraw.movement_ticks = row.user_movement_ticks;
        if (const auto it_cap = capital.user_total_capital.find(uid);
            it_cap != capital.user_total_capital.end()) {
          uraw.deployed_cells = it_cap->second;
        } else {
          uraw.deployed_cells = 0;
        }
        uraw.alpha_bootstrap_default = out.params.alpha_bootstrap_default;
        std::optional<economy::EconomyUserSnapshot> prev_u;
        auto it_prev = last_closed_users_.find(uid);
        if (it_prev != last_closed_users_.end()) prev_u = it_prev->second;
        out.user = economy_engine::ComputeUser(uraw, prev_u, user->balance_mi, out.global.y, current_period_id_, uid);
      }
    }
    world::WorldSnapshot world_snap;
    bool has_world_snapshot = false;
    if (stabilization_actions_.current_world_snapshot) {
      world_snap = stabilization_actions_.current_world_snapshot();
      has_world_snapshot = true;
    }
    out.stabilization = BuildCanonicalSpatialDerived(
        out.params, out.global.m, out.k_snakes, has_world_snapshot ? std::optional<world::WorldSnapshot>(world_snap) : std::nullopt);
    out.stabilization_runtime = stabilization_engine_.runtime_state();
    const auto now = std::chrono::steady_clock::now();
    if (next_fast_check_at_ > now) {
      out.next_fast_check_in_seconds = static_cast<int64_t>(
          std::chrono::duration_cast<std::chrono::seconds>(next_fast_check_at_ - now).count());
    } else {
      out.next_fast_check_in_seconds = 0;
    }
    return out;
  }

  storage::IStorage& storage_;
  economy::PeriodConfig period_cfg_;
  int flush_interval_sec_ = 10;
  economy::StabilizationConfig stabilization_cfg_{};
  economy::StabilizationEngine stabilization_engine_;
  int64_t fast_check_interval_ms_ = 1000;
  std::chrono::steady_clock::time_point next_fast_check_at_{};
  StabilizationActions stabilization_actions_{};
  int cache_ttl_ms_ = 2000;
  mutex mu_;
  bool cache_valid_ = false;
  chrono::steady_clock::time_point cache_expire_at_{};
  std::optional<int> cache_user_id_{};
  Snapshot cache_{};
  std::string current_period_id_;
  int64_t current_period_ends_in_seconds_ = 0;
  int64_t pending_harvested_food_ = 0;
  int64_t pending_real_output_ = 0;
  int64_t pending_movement_ticks_ = 0;
  std::unordered_map<int, int64_t> pending_user_harvested_;
  std::unordered_map<int, int64_t> pending_user_real_output_;
  std::unordered_map<int, int64_t> pending_user_movement_;
  chrono::steady_clock::time_point last_flush_at_{chrono::steady_clock::now()};
  std::optional<economy::EconomySnapshot> last_closed_global_;
  std::unordered_map<std::string, economy::EconomyUserSnapshot> last_closed_users_;
};

static optional<string> get_json_string_field(const string& body, const string& key) {
  const string pat = "\"" + key + "\"";
  size_t p = body.find(pat);
  if (p == string::npos) return nullopt;
  p = body.find(':', p);
  if (p == string::npos) return nullopt;
  ++p;
  while (p < body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
  if (p >= body.size() || body[p] != '"') return nullopt;
  ++p;
  size_t e = body.find('"', p);
  if (e == string::npos) return nullopt;
  return body.substr(p, e - p);
}

static optional<int> get_json_int_field(const string& body, const string& key) {
  const string pat = "\"" + key + "\"";
  size_t p = body.find(pat);
  if (p == string::npos) return nullopt;
  p = body.find(':', p);
  if (p == string::npos) return nullopt;
  ++p;
  while (p < body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
  size_t e = p;
  while (e < body.size() && (isdigit(static_cast<unsigned char>(body[e])) || body[e] == '-')) ++e;
  if (e == p) return nullopt;
  return stoi(body.substr(p, e - p));
}

static optional<double> get_json_double_field(const string& body, const string& key) {
  const string pat = "\"" + key + "\"";
  size_t p = body.find(pat);
  if (p == string::npos) return nullopt;
  p = body.find(':', p);
  if (p == string::npos) return nullopt;
  ++p;
  while (p < body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
  size_t e = p;
  bool seen_digit = false;
  if (e < body.size() && (body[e] == '-' || body[e] == '+')) ++e;
  while (e < body.size()) {
    const char c = body[e];
    if (isdigit(static_cast<unsigned char>(c))) {
      seen_digit = true;
      ++e;
      continue;
    }
    if (c == '.' || c == 'e' || c == 'E' || c == '-' || c == '+') {
      ++e;
      continue;
    }
    break;
  }
  if (!seen_digit || e == p) return nullopt;
  try {
    return stod(body.substr(p, e - p));
  } catch (...) {
    return nullopt;
  }
}

static optional<bool> get_json_bool_field(const string& body, const string& key) {
  const string pat = "\"" + key + "\"";
  size_t p = body.find(pat);
  if (p == string::npos) return nullopt;
  p = body.find(':', p);
  if (p == string::npos) return nullopt;
  ++p;
  while (p < body.size() && isspace(static_cast<unsigned char>(body[p]))) ++p;
  if (body.compare(p, 4, "true") == 0) return true;
  if (body.compare(p, 5, "false") == 0) return false;
  return nullopt;
}

static string normalize_name(const string& in) {
  string out;
  out.reserve(in.size());
  size_t start = 0;
  while (start < in.size() && isspace(static_cast<unsigned char>(in[start]))) ++start;
  size_t end = in.size();
  while (end > start && isspace(static_cast<unsigned char>(in[end - 1]))) --end;
  for (size_t i = start; i < end; ++i) {
    out.push_back(static_cast<char>(tolower(static_cast<unsigned char>(in[i]))));
  }
  return out;
}

static bool is_valid_game_name(const string& in) {
  static const std::regex re(R"(^[A-Za-z][A-Za-z0-9_-]{2,23}$)");
  return std::regex_match(in, re);
}

struct GoogleIdentity {
  string subject;
  string email;
  string issuer;
  string audience;
  int64_t exp = 0;
};

static optional<GoogleIdentity> parse_google_identity_claims(const string& payload) {
  GoogleIdentity out;
  out.subject = get_json_string_field(payload, "sub").value_or("");
  out.email = get_json_string_field(payload, "email").value_or("");
  out.issuer = get_json_string_field(payload, "iss").value_or("");
  out.audience = get_json_string_field(payload, "aud").value_or("");
  out.exp = static_cast<int64_t>(get_json_int_field(payload, "exp").value_or(0));
  if (out.exp <= 0) {
    const auto exp_str = get_json_string_field(payload, "exp");
    if (exp_str.has_value()) {
      try {
        out.exp = std::stoll(*exp_str);
      } catch (...) {
        out.exp = 0;
      }
    }
  }
  if (out.subject.empty() || out.audience.empty() || out.issuer.empty() || out.exp <= 0) return nullopt;
  return out;
}

static bool is_google_issuer_allowed(const string& iss) {
  return iss == "https://accounts.google.com" || iss == "accounts.google.com";
}

static bool read_file_bytes(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) return false;
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return true;
}

static bool is_safe_static_subpath(const std::string& rel) {
  if (rel.empty()) return false;
  if (rel.find("..") != std::string::npos) return false;
  if (rel.front() == '/' || rel.front() == '\\') return false;
  return rel.find('\\') == std::string::npos;
}

static std::string content_type_for_path(const std::string& path) {
  auto dot = path.find_last_of('.');
  const std::string ext = (dot == std::string::npos) ? "" : path.substr(dot);
  if (ext == ".html") return "text/html; charset=utf-8";
  if (ext == ".css") return "text/css; charset=utf-8";
  if (ext == ".js") return "application/javascript; charset=utf-8";
  if (ext == ".json") return "application/json";
  if (ext == ".png") return "image/png";
  if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
  if (ext == ".svg") return "image/svg+xml";
  if (ext == ".webp") return "image/webp";
  if (ext == ".ico") return "image/x-icon";
  return "application/octet-stream";
}

static optional<GoogleIdentity> verify_google_id_token_with_google(const string& id_token) {
  Aws::Client::ClientConfiguration client_cfg;
  client_cfg.scheme = Aws::Http::Scheme::HTTPS;
  client_cfg.connectTimeoutMs = 2500;
  client_cfg.requestTimeoutMs = 4000;
  const Aws::String encoded = Aws::Utils::StringUtils::URLEncode(id_token.c_str());
  Aws::Http::URI uri(Aws::String("https://oauth2.googleapis.com/tokeninfo?id_token=") + encoded);
  auto request = Aws::Http::CreateHttpRequest(
      uri,
      Aws::Http::HttpMethod::HTTP_GET,
      Aws::Utils::Stream::DefaultResponseStreamFactoryMethod);
  if (!request) return nullopt;
  auto client = Aws::Http::CreateHttpClient(client_cfg);
  if (!client) return nullopt;
  auto response = client->MakeRequest(request);
  if (!response || response->GetResponseCode() != Aws::Http::HttpResponseCode::OK) return nullopt;

  std::stringstream ss;
  ss << response->GetResponseBody().rdbuf();
  const string payload = ss.str();
  return parse_google_identity_claims(payload);
}

static bool origin_allowed_for_ws(const string& origin) {
  if (origin.empty()) return false;
  auto starts_with = [](const string& s, const string& p) {
    return s.rfind(p, 0) == 0;
  };
  // Local file:// pages send Origin: null in browsers; allow for local dev mode.
  if (origin == "null") return true;
  if (origin == "https://terrariumsnake.com") return true;
  if (starts_with(origin, "http://127.0.0.1")) return true;
  if (starts_with(origin, "http://localhost")) return true;
  return false;
}

static protocol::Snapshot to_protocol_snapshot(const world::WorldSnapshot& snap_in) {
  protocol::Snapshot snap;
  snap.tick = snap_in.tick;
  snap.w = snap_in.w;
  snap.h = snap_in.h;

  snap.foods.reserve(snap_in.foods.size());
  for (const auto& f : snap_in.foods) {
    snap.foods.push_back(protocol::Vec2{f.x, f.y});
  }

  snap.snakes.reserve(snap_in.snakes.size());
  for (const auto& s : snap_in.snakes) {
    protocol::SnakeState out;
    out.id = s.id;
    out.user_id = s.user_id;
    out.color = s.color;
    out.dir = static_cast<int>(s.dir);
    out.paused = s.paused;
    out.body.reserve(s.body.size());
    for (const auto& p : s.body) {
      out.body.push_back(protocol::Vec2{p.x, p.y});
    }
    snap.snakes.push_back(std::move(out));
  }

  return snap;
}

static string state_to_json(const world::WorldSnapshot& gs) {
  return protocol::encode_snapshot_json(to_protocol_snapshot(gs));
}

static optional<int> require_auth_user(AuthState& auth, const httplib::Request& req) {
  auto it = req.headers.find("Authorization");
  if (it == req.headers.end()) return nullopt;
  const string& v = it->second;
  const string prefix = "Bearer ";
  if (v.rfind(prefix, 0) != 0) return nullopt;
  return auth.token_to_user(v.substr(prefix.size()));
}

static bool require_admin_token(const httplib::Request& req, const std::string& admin_token) {
  if (admin_token.empty()) return false;
  auto header = req.get_header_value("X-Admin-Token");
  if (!header.empty() && header == admin_token) return true;
  if (req.has_param("token")) return req.get_param_value("token") == admin_token;
  return false;
}

static optional<int> user_login(storage::IStorage& storage, AuthState& auth, const string& username, const string& password, string& out_token) {
  auto u = storage.GetUserByUsername(username);
  if (!u || u->password_hash != password) return nullopt;

  int uid = 0;
  try {
    uid = stoi(u->user_id);
  } catch (...) {
    return nullopt;
  }

  out_token = auth.issue_token(uid);
  return uid;
}

static void add_cors(httplib::Response& res) {
  res.set_header("Access-Control-Allow-Origin", "*");
  res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Admin-Token");
}

static bool ensure_user(storage::IStorage& storage, const string& user_id, const string& username, const string& password) {
  auto existing = storage.GetUserById(user_id);
  if (existing.has_value()) return true;

  storage::User u;
  u.user_id = user_id;
  u.username = username;
  u.password_hash = password;
  u.balance_mi = 0;
  u.created_at = static_cast<int64_t>(time(nullptr));
  return storage.PutUser(u);
}

static void seed(storage::IStorage& storage, GameService& game) {
  if (!ensure_user(storage, "1", "user1", "pass1") || !ensure_user(storage, "2", "user2", "pass2")) {
    cerr << "Failed to seed users into DynamoDB\n";
    return;
  }

  game.load_from_storage_or_seed_positions();
  if (game.list_user_snakes(1).empty()) game.create_snake_for_user(1, "#00ff00");
  if (game.list_user_snakes(2).empty()) game.create_snake_for_user(2, "#00aaff");
  game.flush_persistence_delta();
  game.load_from_storage_or_seed_positions();
  cout << "Seeded users: user1/pass1, user2/pass2 (1 snake each)\n";
}

int main(int argc, char** argv) {
  const string mode = (argc >= 2) ? argv[1] : "serve";

  Aws::SDKOptions aws_options;
  Aws::InitAPI(aws_options);

  RuntimeConfig runtime_cfg = RuntimeConfig::FromEnv();
  const std::string admin_token = []() {
    const char* v = std::getenv("ADMIN_TOKEN");
    return (v && *v) ? std::string(v) : std::string{};
  }();

  int grid_w = DEFAULT_W;
  int grid_h = DEFAULT_H;
  int max_snakes_per_user = 3;
  string bind_host = "127.0.0.1";
  int bind_port = 8080;

  const char* envW = getenv("SNAKE_W");
  const char* envH = getenv("SNAKE_H");
  const char* envMax = getenv("SNAKE_MAX_PER_USER");
  const char* envBindHost = getenv("SERVER_BIND_HOST");
  const char* envBindPort = getenv("SERVER_BIND_PORT");

  if (envW) grid_w = max(10, atoi(envW));
  if (envH) grid_h = max(10, atoi(envH));
  if (envMax) max_snakes_per_user = max(1, atoi(envMax));
  if (envBindHost && *envBindHost) bind_host = envBindHost;
  if (envBindPort) bind_port = max(1, atoi(envBindPort));

  cout << "RuntimeConfig: "
       << "TICK_HZ=" << runtime_cfg.tick_hz
       << ", SPECTATOR_HZ=" << runtime_cfg.spectator_hz
       << ", PLAYER_HZ=" << runtime_cfg.player_hz
       << ", ENABLE_BROADCAST=" << (runtime_cfg.enable_broadcast ? "true" : "false")
       << ", DEBUG_TPS=" << (runtime_cfg.debug_tps ? "true" : "false")
       << ", CHUNK_SIZE=" << runtime_cfg.chunk_size
       << ", AOI_RADIUS=" << runtime_cfg.aoi_radius
       << ", SINGLE_CHUNK_MODE=" << (runtime_cfg.single_chunk_mode ? "true" : "false")
       << ", AOI_ENABLED=" << (runtime_cfg.aoi_enabled ? "true" : "false")
       << ", PUBLIC_VIEW_ENABLED=" << (runtime_cfg.public_view_enabled ? "true" : "false")
       << ", PUBLIC_SPECTATOR_HZ=" << runtime_cfg.public_spectator_hz
       << ", AUTH_SPECTATOR_HZ=" << runtime_cfg.auth_spectator_hz
       << ", PUBLIC_CAMERA_SWITCH_TICKS=" << runtime_cfg.public_camera_switch_ticks
       << ", PUBLIC_AOI_RADIUS=" << runtime_cfg.public_aoi_radius
       << ", AUTH_AOI_RADIUS=" << runtime_cfg.auth_aoi_radius
       << ", AOI_PAD_CHUNKS=" << runtime_cfg.aoi_pad_chunks
       << ", CAMERA_MSG_MAX_HZ=" << runtime_cfg.camera_msg_max_hz
       << ", MAX_BORROW_PER_CALL=" << runtime_cfg.max_borrow_per_call
       << ", FOOD_REWARD_CELLS=" << runtime_cfg.food_reward_cells
       << ", RESIZE_THRESHOLD=" << runtime_cfg.resize_threshold
       << ", WORLD_ASPECT_RATIO=" << runtime_cfg.world_aspect_ratio
       << ", WORLD_MASK_MODE=" << runtime_cfg.world_mask_mode
       << ", WORLD_MASK_SEED=" << runtime_cfg.world_mask_seed
       << ", WORLD_MASK_STYLE=" << runtime_cfg.world_mask_style
       << ", ECON_PERIOD_SECONDS=" << runtime_cfg.econ_period_seconds
       << ", ECON_PERIOD_TZ=" << runtime_cfg.econ_period_tz
       << ", ECON_PERIOD_ALIGN=" << runtime_cfg.econ_period_align
       << ", ECONOMY_FLUSH_SECONDS=" << runtime_cfg.economy_flush_seconds
       << ", ECONOMY_PERIOD_HISTORY_DAYS=" << runtime_cfg.economy_period_history_days
       << ", AUTO_EXPANSION_ENABLED=" << (runtime_cfg.auto_expansion_enabled ? "true" : "false")
       << ", AUTO_EXPANSION_TRIGGER_RATIO=" << runtime_cfg.auto_expansion_trigger_ratio
       << ", TARGET_SPATIAL_RATIO=" << runtime_cfg.target_spatial_ratio
       << ", AUTO_EXPANSION_CHECKS_PER_PERIOD=" << runtime_cfg.auto_expansion_checks_per_period
       << ", TARGET_LCR=" << runtime_cfg.target_lcr
       << ", LCR_STRESS_THRESHOLD=" << runtime_cfg.lcr_stress_threshold
       << ", MAX_AUTO_MONEY_GROWTH=" << runtime_cfg.max_auto_money_growth
       << ", PERSISTENCE_PROFILE=" << runtime_cfg.persistence_profile
       << ", PERSISTENCE_SQLITE_PATH=" << runtime_cfg.persistence_sqlite_path
       << ", GOOGLE_AUTH_ENABLED=" << (runtime_cfg.google_auth_enabled ? "true" : "false")
       << ", STARTER_LIQUID_ASSETS=" << runtime_cfg.starter_liquid_assets
       << ", AUTO_SEED_ON_START=" << (runtime_cfg.auto_seed_on_start ? "true" : "false")
       << "\n";

  unique_ptr<storage::IStorage> storage;
  try {
    storage = storage::CreateStorageFromEnv();
  } catch (const exception& e) {
    cerr << "Storage config error: " << e.what() << "\n";
    Aws::ShutdownAPI(aws_options);
    return 1;
  }

  if (!storage->HealthCheck()) {
    cerr << "Storage health check failed\n";
    Aws::ShutdownAPI(aws_options);
    return 1;
  }

  // Ensure an active economy policy row exists for read/write paths and CLI tooling.
  if (!storage->GetEconomyParamsActive().has_value()) {
    storage::EconomyParams defaults;
    defaults.version = 1;
    defaults.food_spawn_target = DEFAULT_FOOD_COUNT;
    defaults.alpha_bootstrap_default = 0.5;
    defaults.updated_at = static_cast<int64_t>(time(nullptr));
    defaults.updated_by = "bootstrap";
    if (!storage->PutEconomyParamsActiveAndVersioned(defaults, "bootstrap")) {
      cerr << "Failed to initialize active economy params\n";
      Aws::ShutdownAPI(aws_options);
      return 1;
    }
  }

  const auto active_params = storage->GetEconomyParamsActive().value_or(storage::EconomyParams{});
  const int food_spawn_target = std::max(1, active_params.food_spawn_target);
  persistence::RuntimeStateStore runtime_store;
  persistence::BufferedSqliteStore buffered_store(runtime_cfg.persistence_sqlite_path);
  persistence::PermanentDynamoStore permanent_store(*storage);
  persistence::ProfilePolicyRegistry policy_registry(runtime_cfg.persistence_profile,
                                                     runtime_cfg.persistence_sqlite_retention_hours);
  persistence::PersistenceRouter router(policy_registry);
  persistence::CoordinatorConfig coordinator_cfg;
  coordinator_cfg.sqlite_retention_hours = runtime_cfg.persistence_sqlite_retention_hours;
  coordinator_cfg.sqlite_max_mb = runtime_cfg.persistence_sqlite_max_mb;
  coordinator_cfg.flush_chunks_seconds = runtime_cfg.persistence_flush_chunks_seconds;
  coordinator_cfg.flush_snapshots_seconds = runtime_cfg.persistence_flush_snapshots_seconds;
  coordinator_cfg.flush_period_deltas_seconds = runtime_cfg.persistence_flush_period_deltas_seconds;
  persistence::PersistenceCoordinator persistence_coordinator(
      coordinator_cfg, runtime_store, buffered_store, permanent_store, policy_registry, router);
  persistence_coordinator.Start();

  GameService game(*storage, persistence_coordinator, grid_w, grid_h, food_spawn_target, max_snakes_per_user);
  game.configure_chunking(runtime_cfg.chunk_size, runtime_cfg.single_chunk_mode);
  game.set_duel_delay_ticks(runtime_cfg.tick_hz);
  game.set_aoi_pad_chunks(runtime_cfg.aoi_pad_chunks);
  game.configure_mask(runtime_cfg.world_mask_mode, runtime_cfg.world_mask_seed, runtime_cfg.world_mask_style);
  EconomyService economy(*storage, runtime_cfg);
  SystemMessageBus system_message_bus;
  if (runtime_cfg.auto_seed_on_start) {
    seed(*storage, game);
  } else {
    game.load_from_storage_or_seed_positions();
  }
  {
    const auto eco = economy.GetState();
    game.set_playable_cell_target(economy_world_area(eco.params, eco.global));
  }
  game.flush_persistence_delta();
  persistence_coordinator.FlushNow();

  auto maybe_resize_world_from_economy = [&](const EconomyService::Snapshot& eco) {
    const auto current = game.snapshot();
    const int64_t current_area = static_cast<int64_t>(current.w) * static_cast<int64_t>(current.h);
    const int64_t target_area = economy_world_area(eco.params, eco.global);
    game.set_playable_cell_target(target_area);
    if (current_area <= 0) return;
    const double rel_diff = std::abs(static_cast<double>(target_area - current_area) / static_cast<double>(current_area));
    if (rel_diff < runtime_cfg.resize_threshold) return;
    const auto [new_w, new_h] = dims_from_area(target_area, runtime_cfg.world_aspect_ratio);
    game.resize_world(new_w, new_h);
    game.set_playable_cell_target(target_area);
    game.flush_persistence_delta();
    persistence_coordinator.FlushNow();
  };

  EconomyService::StabilizationActions stabilization_actions;
  stabilization_actions.expand_playable_cells = [&](int64_t cells) -> int64_t {
    const int64_t expanded = game.expand_playable_cells(cells, runtime_cfg.world_aspect_ratio);
    if (expanded > 0) {
      game.flush_persistence_delta();
      persistence_coordinator.FlushNow();
    }
    return expanded;
  };
  stabilization_actions.emit_system_message = [&](const std::string& dedupe_key,
                                                  const std::string& level,
                                                  const std::string& text) {
    (void)dedupe_key;
    system_message_bus.Publish(level, text);
  };
  stabilization_actions.current_world_snapshot = [&]() { return game.snapshot(); };
  economy.SetStabilizationActions(std::move(stabilization_actions));

  if (mode == "reset") {
    if (!storage->ResetForDev()) {
      cerr << "Dynamo reset failed\n";
      persistence_coordinator.Stop();
      Aws::ShutdownAPI(aws_options);
      return 1;
    }
    cout << "DynamoDB reset complete.\n";
    persistence_coordinator.Stop();
    Aws::ShutdownAPI(aws_options);
    return 0;
  }
  if (mode == "seed") {
    seed(*storage, game);
    persistence_coordinator.Stop();
    Aws::ShutdownAPI(aws_options);
    return 0;
  }
  if (mode != "serve") {
    cerr << "Usage: ./snake_server [serve|seed|reset]\n";
    persistence_coordinator.Stop();
    Aws::ShutdownAPI(aws_options);
    return 1;
  }

  atomic<bool> running{true};
  mutex snapshot_mu;
  uint64_t snapshot_seq = 1;
  mutex sessions_mu;
  unordered_map<string, ClientSession> sessions;
  mutex public_view_mu;
  PublicViewState public_view;
  unordered_map<long long, int> public_activity_scores;
  auto pack_chunk_key = [](int cx, int cy) -> long long {
    return (static_cast<long long>(cx) << 32) ^ static_cast<unsigned long long>(static_cast<uint32_t>(cy));
  };
  auto unpack_chunk_key = [](long long key) -> pair<int, int> {
    int cx = static_cast<int>(key >> 32);
    int cy = static_cast<int>(key & 0xffffffff);
    return {cx, cy};
  };

  {
    const auto snap = game.snapshot();
    const int cx = std::max(0, snap.w / 2);
    const int cy = std::max(0, snap.h / 2);
    const auto chunk = game.coord_to_chunk(cx, cy);
    lock_guard<mutex> lock(public_view_mu);
    public_view.camera_x = cx;
    public_view.camera_y = cy;
    public_view.chunk_cx = chunk.cx;
    public_view.chunk_cy = chunk.cy;
    public_view.initialized = true;
  }

  signal(SIGUSR1, on_reload_signal);
  signal(SIGHUP, on_reload_signal);

  thread loop([&] {
    using clock = chrono::steady_clock;
    using ms = chrono::milliseconds;

    const ms tick_dt(runtime_cfg.TickIntervalMs());
    const int max_spectator_hz = max(runtime_cfg.spectator_hz, max(runtime_cfg.auth_spectator_hz, runtime_cfg.public_spectator_hz));
    const ms spectator_dt(max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(max_spectator_hz)))));
    auto next_tick = clock::now() + tick_dt;
    auto next_broadcast = clock::now() + spectator_dt;
    const int max_catch_up_ticks = 3;
    const auto max_lag = tick_dt * 5;

    uint64_t ticks_since_log = 0;
    uint64_t broadcasts_since_log = 0;
    auto next_log_at = clock::now() + chrono::seconds(5);
    auto last_food_debug_log_at = clock::now() - chrono::seconds(10);

    while (running.load()) {
      if (g_reload_requested) {
        g_reload_requested = 0;
        game.load_from_storage_or_seed_positions();
        {
          lock_guard<mutex> lock(snapshot_mu);
          ++snapshot_seq;
        }
      }

      auto now = clock::now();

      int catch_up_ticks = 0;
      while (now >= next_tick && catch_up_ticks < max_catch_up_ticks) {
        game.tick();
        const auto activity = game.flush_persistence_delta_and_credit_food(runtime_cfg.food_reward_cells);
        const bool has_food_activity = activity.harvested_food > 0;
        EconomyService::Snapshot eco_before_food;
        if (has_food_activity) {
          eco_before_food = economy.GetState();
        }
        economy.OnActivity(activity);
        if (has_food_activity && (clock::now() - last_food_debug_log_at) >= chrono::seconds(2)) {
          const auto eco_after_food = economy.GetState();
          std::cerr << "[food] harvested=" << activity.harvested_food
                    << " users_with_harvest=" << activity.harvested_food_by_user.size()
                    << " money_supply_before=" << eco_before_food.global.m
                    << " money_supply_after=" << eco_after_food.global.m
                    << " treasury_before=" << eco_before_food.global.treasury_balance
                    << " treasury_after=" << eco_after_food.global.treasury_balance
                    << " output_before=" << eco_before_food.global.y
                    << " output_after=" << eco_after_food.global.y << "\n";
          last_food_debug_log_at = clock::now();
        }
        ++ticks_since_log;
        ++catch_up_ticks;
        if (runtime_cfg.public_view_enabled) {
          const auto snap = game.snapshot();
          {
            lock_guard<mutex> lock(public_view_mu);
            for (const auto& s : snap.snakes) {
              if (s.body.empty()) continue;
              const auto cid = game.coord_to_chunk(s.body.front().x, s.body.front().y);
              const int cx = cid.cx;
              const int cy = cid.cy;
              public_activity_scores[pack_chunk_key(cx, cy)] += 1;
            }
            if (runtime_cfg.public_camera_switch_ticks > 0 &&
                (snap.tick - public_view.last_switch_tick) >= static_cast<uint64_t>(runtime_cfg.public_camera_switch_ticks) &&
                !public_activity_scores.empty()) {
              long long best_key = public_activity_scores.begin()->first;
              int best_score = public_activity_scores.begin()->second;
              for (const auto& kv : public_activity_scores) {
                if (kv.second > best_score) {
                  best_score = kv.second;
                  best_key = kv.first;
                }
              }
              const auto [best_cx, best_cy] = unpack_chunk_key(best_key);
              const auto center = game.chunk_center_to_world({best_cx, best_cy});
              const int px = center.x;
              const int py = center.y;
              public_view.chunk_cx = best_cx;
              public_view.chunk_cy = best_cy;
              public_view.camera_x = px;
              public_view.camera_y = py;
              public_view.last_switch_tick = snap.tick;
              public_view.initialized = true;
              public_activity_scores.clear();
            }
          }
        }
        next_tick += tick_dt;
        now = clock::now();
      }

      if ((now - next_tick) > max_lag) {
        next_tick = now + tick_dt;
      }

      while (runtime_cfg.enable_broadcast && now >= next_broadcast) {
        {
          lock_guard<mutex> lock(snapshot_mu);
          ++snapshot_seq;
        }
        ++broadcasts_since_log;
        next_broadcast += spectator_dt;
        now = clock::now();
      }

      if ((now - next_broadcast) > (spectator_dt * 5)) {
        next_broadcast = now + spectator_dt;
      }

      economy.TickStabilization();

      if (runtime_cfg.debug_tps && now >= next_log_at) {
        cout << "[rate] ticks/5s=" << ticks_since_log << ", broadcasts/5s=" << broadcasts_since_log << "\n";
        ticks_since_log = 0;
        broadcasts_since_log = 0;
        next_log_at += chrono::seconds(5);
      }

      auto next_deadline = runtime_cfg.enable_broadcast ? min(next_tick, next_broadcast) : next_tick;
      auto max_sleep_until = clock::now() + ms(5);
      this_thread::sleep_until(min(next_deadline, max_sleep_until));
    }
  });

  AuthState auth;
  httplib::Server srv;
  auto compute_subscribed_chunks_count = [&](const ClientSession&) -> int {
    if (!runtime_cfg.aoi_enabled) return -1;  // all-entities mode
    if (runtime_cfg.single_chunk_mode) return 1;
    const int span = (runtime_cfg.auth_aoi_radius + runtime_cfg.aoi_pad_chunks) * 2 + 1;
    return span * span;
  };

  auto get_or_create_session = [&](const string& sid) {
    lock_guard<mutex> lock(sessions_mu);
    auto it = sessions.find(sid);
    if (it == sessions.end()) {
      ClientSession s;
      s.session_id = sid;
      s.camera_x = grid_w / 2;
      s.camera_y = grid_h / 2;
      s.camera_zoom = 1.0;
      s.subscribed_chunks_count = compute_subscribed_chunks_count(s);
      s.updated_at_ms = now_ms();
      it = sessions.emplace(sid, s).first;
    }
    return it->second;
  };

  srv.set_pre_routing_handler([&](const httplib::Request& req, httplib::Response& res) {
    const auto upgrade = req.get_header_value("Upgrade");
    const bool is_ws_upgrade = !upgrade.empty() &&
                               std::equal(upgrade.begin(), upgrade.end(), "websocket",
                                          [](char a, char b) { return std::tolower(static_cast<unsigned char>(a)) == b; });
    if (req.path == "/ws" && is_ws_upgrade) {
      const auto origin = req.get_header_value("Origin");
      if (!origin_allowed_for_ws(origin)) {
        res.status = 403;
        res.set_content("{\"error\":\"forbidden_origin\"}", "application/json");
        return httplib::Server::HandlerResponse::Handled;
      }
    }
    return httplib::Server::HandlerResponse::Unhandled;
  });

  srv.WebSocket("/ws", [&](const httplib::Request& req, httplib::ws::WebSocket& ws) {
    (void)req;
    const string sid = rand_token(16);
    {
      lock_guard<mutex> lock(sessions_mu);
      ClientSession s;
      s.session_id = sid;
      s.camera_x = grid_w / 2;
      s.camera_y = grid_h / 2;
      s.camera_zoom = 1.0;
      s.is_watcher = true;
      s.subscribed_chunks_count = compute_subscribed_chunks_count(s);
      s.updated_at_ms = now_ms();
      sessions[sid] = s;
    }

    atomic<bool> alive{true};
    thread reader([&] {
      while (alive.load() && ws.is_open()) {
        string msg;
        auto rr = ws.read(msg);
        if (rr != httplib::ws::Text) break;

        auto type = get_json_string_field(msg, "type");
        if (!type) continue;

        if (*type == "auth") {
          auto token = get_json_string_field(msg, "token");
          if (!token || token->empty()) {
            ws.send("{\"type\":\"auth_ack\",\"channel\":\"private\",\"ok\":false}");
            continue;
          }
          auto uid = auth.token_to_user(*token);
          if (!uid) {
            ws.send("{\"type\":\"auth_ack\",\"channel\":\"private\",\"ok\":false}");
            continue;
          }
          {
            lock_guard<mutex> lock(sessions_mu);
            auto it = sessions.find(sid);
            if (it != sessions.end()) {
              it->second.auth_user_id = *uid;
              it->second.is_watcher = false;
              it->second.updated_at_ms = now_ms();
            }
          }
          ws.send("{\"type\":\"auth_ack\",\"channel\":\"private\",\"ok\":true}");
          continue;
        }

        ClientSession session = get_or_create_session(sid);
        if (!session.auth_user_id.has_value()) continue;

        if (*type == "input") {
          auto snake_id = get_json_int_field(msg, "snake_id");
          auto dir = get_json_string_field(msg, "dir");
          auto pause_toggle = get_json_bool_field(msg, "pause_toggle");
          if (snake_id && dir) {
            int d = 0;
            if (*dir == "L") d = 1;
            else if (*dir == "R") d = 2;
            else if (*dir == "U") d = 3;
            else if (*dir == "D") d = 4;
            if (d >= 1 && d <= 4) {
              game.set_snake_dir(*session.auth_user_id, *snake_id, static_cast<world::Dir>(d));
            }
          }
          if (snake_id && pause_toggle && *pause_toggle) {
            game.toggle_snake_pause(*session.auth_user_id, *snake_id);
          }
          continue;
        }

        if (*type == "camera_set") {
          auto x = get_json_int_field(msg, "x");
          auto y = get_json_int_field(msg, "y");
          auto zoom = get_json_double_field(msg, "zoom");
          if (!x || !y) continue;
          const uint64_t now_millis = now_ms();
          const uint64_t min_gap = static_cast<uint64_t>(
              max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(runtime_cfg.camera_msg_max_hz)))));
          if (session.last_camera_update_ms != 0 && now_millis - session.last_camera_update_ms < min_gap) continue;
          session.camera_x = max(0, min(game.snapshot().w - 1, *x));
          session.camera_y = max(0, min(game.snapshot().h - 1, *y));
          if (zoom) session.camera_zoom = max(0.25, min(4.0, *zoom));
          auto follow_id = get_json_int_field(msg, "follow_snake_id");
          if (follow_id && *follow_id > 0) session.watched_snake_id = *follow_id;
          session.subscribed_chunks_count = compute_subscribed_chunks_count(session);
          session.last_camera_update_ms = now_millis;
          session.updated_at_ms = now_millis;
          lock_guard<mutex> lock(sessions_mu);
          sessions[sid] = session;
        }

        if (*type == "public_camera_init") {
          // Spectator one-shot init: accepts only from unauthenticated sessions.
          if (session.auth_user_id.has_value()) continue;
          auto x = get_json_int_field(msg, "x");
          auto y = get_json_int_field(msg, "y");
          if (!x || !y) continue;
          const auto snap = game.snapshot();
          const int cx = max(0, min(snap.w - 1, *x));
          const int cy = max(0, min(snap.h - 1, *y));
          const auto chunk = game.coord_to_chunk(cx, cy);
          {
            lock_guard<mutex> lock(public_view_mu);
            if (!public_view.initialized) {
              public_view.camera_x = cx;
              public_view.camera_y = cy;
              public_view.chunk_cx = chunk.cx;
              public_view.chunk_cy = chunk.cy;
              public_view.initialized = true;
            }
          }
          continue;
        }
      }
      alive.store(false);
    });

    auto next_world_send = chrono::steady_clock::now();
    auto next_economy_send = chrono::steady_clock::now();
    auto next_private_send = chrono::steady_clock::now();
    auto next_system_send = chrono::steady_clock::now();
    uint64_t last_system_message_id = 0;

    while (alive.load() && ws.is_open()) {
      ClientSession session = get_or_create_session(sid);
      const bool is_auth = session.auth_user_id.has_value();
      const int hz = is_auth ? runtime_cfg.auth_spectator_hz : runtime_cfg.public_spectator_hz;
      const auto world_dt = chrono::milliseconds(max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(hz)))));

      auto now = chrono::steady_clock::now();
      if (now >= next_world_send) {
        world::WorldSnapshot snap;
        string channel = "public";
        int cam_x = 0;
        int cam_y = 0;
        int aoi_radius = runtime_cfg.public_aoi_radius;
        string mode = "PUBLIC";
        const int padded_public_radius = aoi_radius + runtime_cfg.aoi_pad_chunks;
        int aoi_chunks = runtime_cfg.single_chunk_mode ? 1 : (padded_public_radius * 2 + 1) * (padded_public_radius * 2 + 1);
        int public_chunk_cx = 0;
        int public_chunk_cy = 0;

        if (is_auth) {
          channel = "private";
          mode = "AUTH";
          cam_x = session.camera_x;
          cam_y = session.camera_y;
          aoi_radius = runtime_cfg.auth_aoi_radius;
          aoi_chunks = compute_subscribed_chunks_count(session);
        } else {
          PublicViewState pv;
          {
            lock_guard<mutex> lock(public_view_mu);
            pv = public_view;
          }
          if (!pv.initialized) {
            const auto world_snap = game.snapshot();
            const int cx = std::max(0, world_snap.w / 2);
            const int cy = std::max(0, world_snap.h / 2);
            const auto chunk = game.coord_to_chunk(cx, cy);
            {
              lock_guard<mutex> lock(public_view_mu);
              public_view.camera_x = cx;
              public_view.camera_y = cy;
              public_view.chunk_cx = chunk.cx;
              public_view.chunk_cy = chunk.cy;
              public_view.initialized = true;
              pv = public_view;
            }
          }
          cam_x = pv.camera_x;
          cam_y = pv.camera_y;
          public_chunk_cx = pv.chunk_cx;
          public_chunk_cy = pv.chunk_cy;
        }

        snap = game.snapshot_for_camera(cam_x, cam_y, runtime_cfg.aoi_enabled, aoi_radius, runtime_cfg.debug_tps);
        int aoi_min_x = 0;
        int aoi_max_x = 0;
        int aoi_min_y = 0;
        int aoi_max_y = 0;
        int cam_chunk_x = 0;
        int cam_chunk_y = 0;
        int effective_radius = std::max(0, aoi_radius + runtime_cfg.aoi_pad_chunks);
        if (!runtime_cfg.single_chunk_mode) {
          const int cs = std::max(1, runtime_cfg.chunk_size);
          const int chunks_x = std::max(1, (snap.w + cs - 1) / cs);
          const int chunks_y = std::max(1, (snap.h + cs - 1) / cs);
          cam_chunk_x = std::max(0, std::min(chunks_x - 1, cam_x / cs));
          cam_chunk_y = std::max(0, std::min(chunks_y - 1, cam_y / cs));
          if (!runtime_cfg.aoi_enabled) {
            aoi_min_x = 0;
            aoi_max_x = chunks_x - 1;
            aoi_min_y = 0;
            aoi_max_y = chunks_y - 1;
          } else {
            aoi_min_x = std::max(0, cam_chunk_x - effective_radius);
            aoi_max_x = std::min(chunks_x - 1, cam_chunk_x + effective_radius);
            aoi_min_y = std::max(0, cam_chunk_y - effective_radius);
            aoi_max_y = std::min(chunks_y - 1, cam_chunk_y + effective_radius);
          }
        }

        const string snap_json = state_to_json(snap);
        ostringstream out;
        out << "{"
            << "\"type\":\"world_snapshot\","
            << "\"channel\":\"" << channel << "\","
            << "\"mode\":\"" << mode << "\","
            << "\"camera\":{\"x\":" << cam_x << ",\"y\":" << cam_y << ",\"zoom\":" << json_number(session.camera_zoom) << "},"
            << "\"aoi\":{"
            << "\"min_chunk_x\":" << aoi_min_x << ","
            << "\"max_chunk_x\":" << aoi_max_x << ","
            << "\"min_chunk_y\":" << aoi_min_y << ","
            << "\"max_chunk_y\":" << aoi_max_y << ","
            << "\"camera_chunk_x\":" << cam_chunk_x << ","
            << "\"camera_chunk_y\":" << cam_chunk_y << ","
            << "\"radius\":" << aoi_radius << ","
            << "\"effective_radius\":" << effective_radius
            << "},"
            << "\"aoi_chunks\":" << aoi_chunks << ","
            << "\"public_camera_chunk\":{\"cx\":" << public_chunk_cx << ",\"cy\":" << public_chunk_cy << "},"
            << "\"chunk_size\":" << runtime_cfg.chunk_size << ","
            << "\"mask\":{"
            << "\"mode\":\"" << json_escape(snap.mask_mode) << "\","
            << "\"style\":\"" << json_escape(snap.mask_style) << "\","
            << "\"seed\":" << snap.mask_seed << ","
            << "\"playable_cells\":" << snap.playable_cells << ","
            << "\"unplayable_cells\":" << snap.unplayable_cells
            << "},"
            << "\"snapshot\":" << snap_json
            << "}";
        if (!ws.send(out.str())) break;
        next_world_send = now + world_dt;
      }

      if (now >= next_economy_send) {
        const auto eco = economy.GetState();
        const std::string stabilization_status = stabilization_status_ui(eco.stabilization_runtime);
        ostringstream out;
        out << "{"
            << "\"type\":\"economy_world\","
            << "\"channel\":\"public\","
            << "\"period_id\":\"" << json_escape(eco.period_id) << "\","
            << "\"Y\":" << eco.global.y << ","
            << "\"extracted_output\":" << eco.global.y << ","
            << "\"K\":" << eco.global.k << ","
            << "\"L\":" << eco.global.l << ","
            << "\"alpha\":" << json_number(eco.global.alpha) << ","
            << "\"A\":" << json_number(eco.global.a) << ","
            << "\"M\":" << eco.global.m << ","
            << "\"P\":" << json_number(eco.global.p) << ","
            << "\"pi\":" << json_number(eco.global.pi) << ","
            << "\"price_index_valid\":" << (eco.global.price_index_valid ? "true" : "false") << ","
            << "\"inflation_valid\":" << (eco.global.inflation_valid ? "true" : "false") << ","
            << "\"treasury_balance\":" << eco.global.treasury_balance << ","
            << "\"field_size\":" << eco.stabilization.field_size << ","
            << "\"free_space_on_field\":" << eco.stabilization.free_space_on_field << ","
            << "\"system_white_space_reserve\":" << eco.stabilization.treasury_white_space << ","
            << "\"spatial_ratio_r\":" << json_number(eco.stabilization.spatial_ratio_r) << ","
            << "\"stabilization_status\":\"" << json_escape(stabilization_status) << "\","
            << "\"period_ends_in_seconds\":" << eco.period_ends_in_seconds << ","
            << "\"snapshot_status\":\"" << json_escape(eco.global.snapshot_status) << "\","
            << "\"A_world\":" << economy_world_area(eco.params, eco.global)
            << "}";
        if (!ws.send(out.str())) break;
        next_economy_send = now + chrono::seconds(1);
      }

      if (is_auth && now >= next_private_send) {
        const auto user = storage->GetUserById(std::to_string(*session.auth_user_id));
        const auto eco_user = economy.GetState(*session.auth_user_id);
        if (user.has_value()) {
          const auto snakes = game.list_user_snakes(*session.auth_user_id);
          int64_t deployed = 0;
          for (const auto& s : snakes) deployed += static_cast<int64_t>(s.body.size());
          ostringstream out;
          out << "{"
              << "\"type\":\"user_state\","
              << "\"channel\":\"private\","
              << "\"user_id\":" << *session.auth_user_id << ","
              << "\"balance_mi\":" << user->balance_mi << ","
              << "\"liquid_assets\":" << user->balance_mi << ","
              << "\"deployed_k\":" << deployed << ","
              << "\"snake_count\":" << snakes.size();
          if (eco_user.user.has_value()) {
            out << ",\"economy_user\":{"
                << "\"Y_u\":" << eco_user.user->y_u << ","
                << "\"extracted_output_u\":" << eco_user.user->y_u << ","
                << "\"K_u\":" << eco_user.user->k_u << ","
                << "\"L_u\":" << eco_user.user->l_u << ","
                << "\"alpha_u\":" << json_number(eco_user.user->alpha_u) << ","
                << "\"A_u\":" << json_number(eco_user.user->a_u) << ","
                << "\"market_share\":" << json_number(eco_user.user->market_share) << ","
                << "\"storage_balance\":" << eco_user.user->storage_balance
                << "}";
          }
          out
              << "}";
          if (!ws.send(out.str())) break;
        }
        next_private_send = now + chrono::seconds(1);
      }

      if (now >= next_system_send) {
        const auto messages = system_message_bus.GetSince(last_system_message_id);
        for (const auto& m : messages) {
          std::ostringstream out;
          out << "{"
              << "\"type\":\"system_message\","
              << "\"id\":" << m.id << ","
              << "\"level\":\"" << json_escape(m.level) << "\","
              << "\"message\":\"" << json_escape(m.text) << "\","
              << "\"created_at\":" << m.created_at
              << "}";
          if (!ws.send(out.str())) {
            alive.store(false);
            break;
          }
          last_system_message_id = m.id;
        }
        next_system_send = now + chrono::milliseconds(250);
      }

      this_thread::sleep_for(chrono::milliseconds(10));
    }

    alive.store(false);
    if (reader.joinable()) reader.join();
    {
      lock_guard<mutex> lock(sessions_mu);
      sessions.erase(sid);
    }
  });

  srv.Options(R"(.*)", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    res.status = 204;
  });

  srv.Get("/game/state", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    res.set_content(state_to_json(game.snapshot()), "application/json");
  });

  srv.Get("/game/runtime", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    ostringstream o;
    o << "{"
      << "\"tick_hz\":" << runtime_cfg.tick_hz << ","
      << "\"spectator_hz\":" << runtime_cfg.spectator_hz << ","
      << "\"player_hz\":" << runtime_cfg.player_hz << ","
      << "\"enable_broadcast\":" << (runtime_cfg.enable_broadcast ? "true" : "false") << ","
      << "\"chunk_size\":" << runtime_cfg.chunk_size << ","
      << "\"aoi_radius\":" << runtime_cfg.aoi_radius << ","
      << "\"aoi_pad_chunks\":" << runtime_cfg.aoi_pad_chunks << ","
      << "\"single_chunk_mode\":" << (runtime_cfg.single_chunk_mode ? "true" : "false") << ","
      << "\"aoi_enabled\":" << (runtime_cfg.aoi_enabled ? "true" : "false") << ","
      << "\"google_auth_enabled\":" << (runtime_cfg.google_auth_enabled ? "true" : "false") << ","
      << "\"google_client_id\":\"" << json_escape(runtime_cfg.google_client_id) << "\""
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    res.set_content("{\"ok\":true}", "application/json");
  });

  // Local/dev fallback static serving so http://127.0.0.1:8080 works without a reverse proxy.
  // In prod, Caddy/Nginx static serving still takes precedence.
  srv.Get("/", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    std::string body;
    if (!read_file_bytes("index.html", body)) {
      res.status = 404;
      res.set_content("{\"error\":\"index_not_found\"}", "application/json");
      return;
    }
    res.set_content(body, "text/html; charset=utf-8");
  });

  srv.Get("/index.html", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    std::string body;
    if (!read_file_bytes("index.html", body)) {
      res.status = 404;
      res.set_content("{\"error\":\"index_not_found\"}", "application/json");
      return;
    }
    res.set_content(body, "text/html; charset=utf-8");
  });

  srv.Get("/assets/world_evolution_log.json", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    std::string body;
    if (!read_file_bytes("assets/world_evolution_log.json", body)) {
      res.status = 404;
      res.set_content("{\"error\":\"world_evolution_log_not_found\"}", "application/json");
      return;
    }
    if (body.empty()) {
      res.status = 500;
      res.set_content("{\"error\":\"world_evolution_log_empty\"}", "application/json");
      return;
    }
    res.set_content(body, "application/json");
  });

  srv.Get(R"(/src/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    const std::string rel = req.matches[1];
    if (!is_safe_static_subpath(rel)) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_path\"}", "application/json");
      return;
    }
    const std::string full = "src/" + rel;
    std::string body;
    if (!read_file_bytes(full, body)) {
      res.status = 404;
      res.set_content("{\"error\":\"not_found\"}", "application/json");
      return;
    }
    res.set_content(body, content_type_for_path(full).c_str());
  });

  srv.Get(R"(/assets/(.+))", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    const std::string rel = req.matches[1];
    if (!is_safe_static_subpath(rel)) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_path\"}", "application/json");
      return;
    }
    const std::string full = "assets/" + rel;
    std::string body;
    if (!read_file_bytes(full, body)) {
      res.status = 404;
      res.set_content("{\"error\":\"not_found\"}", "application/json");
      return;
    }
    res.set_content(body, content_type_for_path(full).c_str());
  });

  srv.Get("/economy/state", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    EconomyService::Snapshot s = economy.GetState();
    const auto period_row = storage->GetEconomyPeriod(s.period_id).value_or(storage::EconomyPeriod{});
    const int64_t a_world = economy_world_area(s.params, s.global);
    const int64_t m_white = std::max<int64_t>(0, a_world - s.global.k);
    ostringstream o;
    o << "{"
      << "\"period_id\":\"" << json_escape(s.period_id) << "\","
      << "\"period_key\":\"" << json_escape(s.period_id) << "\","
      << "\"period_ends_in_seconds\":" << s.period_ends_in_seconds << ","
      << "\"snapshot_status\":\"" << json_escape(s.global.snapshot_status) << "\","
      << "\"is_finalized\":" << (period_row.is_finalized ? "true" : "false") << ","
      << "\"finalized_at\":" << period_row.finalized_at << ","
      << "\"Y\":" << s.global.y << ","
      << "\"extracted_output\":" << s.global.y << ","
      << "\"K\":" << s.global.k << ","
      << "\"L\":" << s.global.l << ","
      << "\"alpha\":" << json_number(s.global.alpha) << ","
      << "\"A\":" << json_number(s.global.a) << ","
      << "\"M\":" << s.global.m << ","
      << "\"P\":" << json_number(s.global.p) << ","
      << "\"pi\":" << json_number(s.global.pi) << ","
      << "\"price_index_valid\":" << (s.global.price_index_valid ? "true" : "false") << ","
      << "\"inflation_valid\":" << (s.global.inflation_valid ? "true" : "false") << ","
      << "\"A_world\":" << a_world << ","
      << "\"M_white\":" << m_white << ","
      << "\"R\":" << json_number(s.stabilization.spatial_ratio_r) << ","
      << "\"LCR\":" << json_number(s.stabilization.lcr) << ","
      << "\"field_size\":" << s.stabilization.field_size << ","
      << "\"free_space_on_field\":" << s.stabilization.free_space_on_field << ","
      << "\"system_white_space_reserve\":" << s.stabilization.treasury_white_space << ","
      << "\"stabilization_status\":\"" << json_escape(stabilization_status_ui(s.stabilization_runtime)) << "\","
      << "\"treasury_white_space\":" << s.stabilization.treasury_white_space << ","
      << "\"treasury_balance\":" << s.global.treasury_balance << ","
      << "\"alpha_bootstrap\":" << (s.global.alpha_bootstrap ? "true" : "false") << ","
      << "\"inputs\":{"
      << "\"k_land\":" << s.params.k_land << ","
      << "\"a_productivity\":" << json_number(s.params.a_productivity) << ","
      << "\"v_velocity\":" << json_number(s.params.v_velocity) << ","
      << "\"m_gov_reserve\":" << s.params.m_gov_reserve << ","
      << "\"cap_delta_m\":" << s.params.cap_delta_m << ","
      << "\"delta_m_issue\":" << s.params.delta_m_issue << ","
      << "\"delta_k_obs\":" << s.params.delta_k_obs
      << "},"
      // Backward-compatible aliases for existing consumers.
      << "\"legacy\":{"
      << "\"k_snakes\":" << s.k_snakes
      << "}"
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/economy/user", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }
    const auto s = economy.GetState(*uid);
    if (!s.user.has_value()) {
      res.status = 404;
      res.set_content("{\"error\":\"economy_user_not_found\"}", "application/json");
      return;
    }
    ostringstream o;
    o << "{"
      << "\"period_id\":\"" << json_escape(s.period_id) << "\","
      << "\"Y_u\":" << s.user->y_u << ","
      << "\"extracted_output_u\":" << s.user->y_u << ","
      << "\"K_u\":" << s.user->k_u << ","
      << "\"L_u\":" << s.user->l_u << ","
      << "\"alpha_u\":" << json_number(s.user->alpha_u) << ","
      << "\"A_u\":" << json_number(s.user->a_u) << ","
      << "\"market_share\":" << json_number(s.user->market_share) << ","
      << "\"storage_balance\":" << s.user->storage_balance << ","
      << "\"alpha_bootstrap\":" << (s.user->alpha_bootstrap ? "true" : "false")
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/economy/debug", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!require_admin_token(req, admin_token)) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized_admin\"}", "application/json");
      return;
    }
    const auto eco = economy.GetState();
    const auto dbg = economy.GetDebugState();
    std::ostringstream o;
    o << "{"
      << "\"period_id\":\"" << json_escape(dbg.period_id) << "\","
      << "\"period_ends_in_seconds\":" << dbg.period_ends_in_seconds << ","
      << "\"pending\":{"
      << "\"harvested_food\":" << dbg.pending_harvested_food << ","
      << "\"real_output\":" << dbg.pending_real_output << ","
      << "\"movement_ticks\":" << dbg.pending_movement_ticks << ","
      << "\"users\":" << dbg.pending_users
      << "},"
      << "\"flush_interval_sec\":" << dbg.flush_interval_sec << ","
      << "\"seconds_since_last_flush\":" << dbg.seconds_since_last_flush << ","
      << "\"stabilization\":{"
      << "\"field_size\":" << eco.stabilization.field_size << ","
      << "\"free_space_on_field\":" << eco.stabilization.free_space_on_field << ","
      << "\"spatial_ratio_r\":" << json_number(eco.stabilization.spatial_ratio_r) << ","
      << "\"lcr\":" << json_number(eco.stabilization.lcr) << ","
      << "\"treasury_white_space\":" << eco.stabilization.treasury_white_space << ","
      << "\"failures_this_period\":" << eco.stabilization_runtime.spatial_expansion_failures_current_period << ","
      << "\"mode\":\"" << json_escape(stabilization_status_ui(eco.stabilization_runtime)) << "\","
      << "\"last_action_period_id\":\"" << json_escape(eco.stabilization_runtime.last_stabilization_action_period_id) << "\","
      << "\"last_action_type\":\"" << json_escape(eco.stabilization_runtime.last_stabilization_action_type) << "\","
      << "\"next_fast_check_in_seconds\":" << eco.next_fast_check_in_seconds
      << "},"
      << "\"price_index_valid\":" << (eco.global.price_index_valid ? "true" : "false") << ","
      << "\"inflation_valid\":" << (eco.global.inflation_valid ? "true" : "false") << ","
      << "\"snapshot_status\":\"" << json_escape(eco.global.snapshot_status) << "\""
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/admin/economy/status", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!require_admin_token(req, admin_token)) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized_admin\"}", "application/json");
      return;
    }
    const auto s = economy.GetState();
    const auto p = storage->GetEconomyPeriod(s.period_id).value_or(storage::EconomyPeriod{});
    const int64_t a_world = economy_world_area(s.params, s.global);
    const std::string stabilization_status = stabilization_status_ui(s.stabilization_runtime);
    std::ostringstream o;
    o << "{"
      << "\"period_id\":\"" << json_escape(s.period_id) << "\","
      << "\"is_finalized\":" << (p.is_finalized ? "true" : "false") << ","
      << "\"finalized_at\":" << p.finalized_at << ","
      << "\"snapshot_status\":\"" << json_escape(s.global.snapshot_status) << "\","
      << "\"Y\":" << s.global.y << ","
      << "\"K\":" << s.global.k << ","
      << "\"L\":" << s.global.l << ","
      << "\"alpha\":" << json_number(s.global.alpha) << ","
      << "\"A\":" << json_number(s.global.a) << ","
      << "\"M\":" << s.global.m << ","
      << "\"P\":" << json_number(s.global.p) << ","
      << "\"pi\":" << json_number(s.global.pi) << ","
      << "\"price_index_valid\":" << (s.global.price_index_valid ? "true" : "false") << ","
      << "\"inflation_valid\":" << (s.global.inflation_valid ? "true" : "false") << ","
      << "\"A_world\":" << a_world << ","
      << "\"M_white\":" << std::max<int64_t>(0, a_world - s.global.k) << ","
      << "\"field_size\":" << s.stabilization.field_size << ","
      << "\"free_space_on_field\":" << s.stabilization.free_space_on_field << ","
      << "\"system_white_space_reserve\":" << s.stabilization.treasury_white_space << ","
      << "\"spatial_ratio_r\":" << json_number(s.stabilization.spatial_ratio_r) << ","
      << "\"lcr\":" << json_number(s.stabilization.lcr) << ","
      << "\"treasury_white_space\":" << s.stabilization.treasury_white_space << ","
      << "\"failures_this_period\":" << s.stabilization_runtime.spatial_expansion_failures_current_period << ","
      << "\"stabilization_mode\":\"" << json_escape(stabilization_status) << "\","
      << "\"stabilization_status\":\"" << json_escape(stabilization_status) << "\","
      << "\"last_stabilization_action_period_id\":\""
      << json_escape(s.stabilization_runtime.last_stabilization_action_period_id) << "\","
      << "\"last_stabilization_action_type\":\""
      << json_escape(s.stabilization_runtime.last_stabilization_action_type) << "\","
      << "\"next_fast_check_in_seconds\":" << s.next_fast_check_in_seconds << ","
      << "\"period_ends_in_seconds\":" << s.period_ends_in_seconds
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post("/admin/economy/recompute", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!require_admin_token(req, admin_token)) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized_admin\"}", "application/json");
      return;
    }
    const bool force_rewrite = get_json_bool_field(req.body, "force_rewrite").value_or(false);
    const std::string period_id = get_json_string_field(req.body, "period_id").value_or(economy.GetState().period_id);
    const auto existing = storage->GetEconomyPeriod(period_id).value_or(storage::EconomyPeriod{});
    if (existing.is_finalized && !force_rewrite) {
      res.status = 409;
      res.set_content("{\"error\":\"period_finalized_use_force_rewrite\"}", "application/json");
      return;
    }
    const auto snap = economy.RecomputeAndPersist(period_id, std::nullopt, force_rewrite);
    maybe_resize_world_from_economy(snap);
    std::ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"period_id\":\"" << json_escape(period_id) << "\","
      << "\"force_rewrite\":" << (force_rewrite ? "true" : "false") << ","
      << "\"Y\":" << snap.global.y << ","
      << "\"K\":" << snap.global.k << ","
      << "\"L\":" << snap.global.l << ","
      << "\"M\":" << snap.global.m << ","
      << "\"P\":" << json_number(snap.global.p) << ","
      << "\"pi\":" << json_number(snap.global.pi)
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post("/admin/economy/set", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!require_admin_token(req, admin_token)) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized_admin\"}", "application/json");
      return;
    }
    const auto param = get_json_string_field(req.body, "param");
    const auto value_s = get_json_string_field(req.body, "value");
    if (!param.has_value() || !value_s.has_value()) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_payload\"}", "application/json");
      return;
    }

    auto p = storage->GetEconomyParamsActive().value_or(storage::EconomyParams{});
    auto to_i64 = [&](const std::string& s, int64_t& out) -> bool {
      try {
        out = std::stoll(s);
        return true;
      } catch (...) {
        return false;
      }
    };
    auto to_d = [&](const std::string& s, double& out) -> bool {
      try {
        out = std::stod(s);
        return true;
      } catch (...) {
        return false;
      }
    };

    int64_t vi = 0;
    double vd = 0.0;
    if (*param == "k_land" && to_i64(*value_s, vi)) p.k_land = static_cast<int>(std::max<int64_t>(1, vi));
    else if (*param == "a_productivity" && to_d(*value_s, vd)) p.a_productivity = std::max(0.0, vd);
    else if (*param == "v_velocity" && to_d(*value_s, vd)) p.v_velocity = std::max(0.0, vd);
    else if (*param == "m_gov_reserve" && to_i64(*value_s, vi)) p.m_gov_reserve = std::max<int64_t>(0, vi);
    else if (*param == "cap_delta_m" && to_i64(*value_s, vi)) p.cap_delta_m = std::max<int64_t>(0, vi);
    else if (*param == "delta_m_issue" && to_i64(*value_s, vi)) p.delta_m_issue = vi;
    else if (*param == "delta_k_obs" && to_i64(*value_s, vi)) p.delta_k_obs = vi;
    else if (*param == "food_spawn_target" && to_i64(*value_s, vi)) p.food_spawn_target = static_cast<int>(std::max<int64_t>(1, vi));
    else if (*param == "alpha_bootstrap_default" && to_d(*value_s, vd)) p.alpha_bootstrap_default = std::max(0.05, std::min(0.95, vd));
    else {
      res.status = 400;
      res.set_content("{\"error\":\"unsupported_param\"}", "application/json");
      return;
    }
    p.updated_at = static_cast<int64_t>(time(nullptr));
    p.updated_by = "admin_api";
    if (!storage->PutEconomyParamsActiveAndVersioned(p, p.updated_by)) {
      res.status = 500;
      res.set_content("{\"error\":\"set_failed\"}", "application/json");
      return;
    }
    economy.InvalidateCache();
    auto snap = economy.GetState();
    maybe_resize_world_from_economy(snap);
    storage::SnakeEvent event;
    event.snake_id = "system";
    event.event_id = std::to_string(static_cast<int64_t>(time(nullptr))) + "#admin_economy_set#" + *param;
    event.event_type = "ADMIN_OVERRIDE_ECONOMY_SET";
    event.created_at = static_cast<int64_t>(time(nullptr));
    (void)storage->AppendSnakeEvent(event);
    res.set_content("{\"ok\":true}", "application/json");
  });

  srv.Post("/admin/treasury/set", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!require_admin_token(req, admin_token)) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized_admin\"}", "application/json");
      return;
    }
    auto amount = get_json_int_field(req.body, "amount");
    if (!amount || *amount < 0) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_amount\"}", "application/json");
      return;
    }
    auto p = storage->GetEconomyParamsActive().value_or(storage::EconomyParams{});
    p.m_gov_reserve = *amount;
    p.updated_at = static_cast<int64_t>(time(nullptr));
    p.updated_by = "admin_api";
    if (!storage->PutEconomyParamsActiveAndVersioned(p, p.updated_by)) {
      res.status = 500;
      res.set_content("{\"error\":\"set_failed\"}", "application/json");
      return;
    }
    economy.InvalidateCache();
    storage::SnakeEvent event;
    event.snake_id = "system";
    event.event_id = std::to_string(static_cast<int64_t>(time(nullptr))) + "#admin_treasury_set";
    event.event_type = "ADMIN_OVERRIDE_TREASURY_SET";
    event.delta_length = p.m_gov_reserve > std::numeric_limits<int>::max() ? std::numeric_limits<int>::max() : static_cast<int>(p.m_gov_reserve);
    event.created_at = static_cast<int64_t>(time(nullptr));
    (void)storage->AppendSnakeEvent(event);
    res.set_content("{\"ok\":true}", "application/json");
  });

  srv.Get("/user/me", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    const auto user = storage->GetUserById(std::to_string(*uid));
    if (!user.has_value()) {
      res.status = 404;
      res.set_content("{\"error\":\"user_not_found\"}", "application/json");
      return;
    }

    std::vector<storage::Snake> snakes;
    for (const auto& s : storage->ListSnakes()) {
      if (s.owner_user_id == std::to_string(*uid)) snakes.push_back(s);
    }
    int64_t deployed = 0;
    for (const auto& s : snakes) deployed += static_cast<int64_t>(std::max(0, s.length_k));

    ostringstream o;
    o << "{"
      << "\"user_id\":" << *uid << ","
      << "\"auth_provider\":\"" << json_escape(user->auth_provider) << "\","
      << "\"onboarding_completed\":" << (user->onboarding_completed ? "true" : "false") << ","
      << "\"company_name\":\"" << json_escape(user->company_name) << "\","
      << "\"starter_snake_id\":\"" << json_escape(user->starter_snake_id) << "\","
      << "\"balance_mi\":" << user->balance_mi << ","
      << "\"liquid_assets\":" << user->balance_mi << ","
      << "\"deployed_k\":" << deployed << ","
      << "\"snake_count\":" << snakes.size() << ","
      << "\"last_seen_world_version\":\"" << json_escape(user->last_seen_world_version) << "\""
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post("/user/world-version-seen", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }
    auto version = get_json_string_field(req.body, "version");
    if (!version.has_value() || !is_strict_semver(*version)) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_version\"}", "application/json");
      return;
    }
    if (!storage->UpdateUserLastSeenWorldVersion(std::to_string(*uid), *version)) {
      res.status = 500;
      res.set_content("{\"error\":\"update_failed\"}", "application/json");
      return;
    }
    res.set_content("{\"ok\":true}", "application/json");
  });

  auto handle_borrow_cells = [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      std::cerr << "[economy_action] action=borrow"
                << " user_id=unknown"
                << " amount=0"
                << " profile=" << runtime_cfg.persistence_profile
                << " intent_type=UserBalanceChanged+TreasuryBalanceChanged"
                << " policy_result=unauthorized"
                << " rejection_reason=unauthorized"
                << " treasury_balance_before=-1\n";
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    auto amount = get_json_int_field(req.body, "amount");
    if (!amount) amount = get_json_int_field(req.body, "cells");
    if (!amount || *amount <= 0 || *amount > runtime_cfg.max_borrow_per_call) {
      std::cerr << "[economy_action] action=borrow"
                << " user_id=" << *uid
                << " amount=" << (amount.has_value() ? *amount : 0)
                << " profile=" << runtime_cfg.persistence_profile
                << " intent_type=UserBalanceChanged+TreasuryBalanceChanged"
                << " policy_result=validation_reject"
                << " rejection_reason=invalid_amount"
                << " treasury_balance_before=-1\n";
      res.status = 400;
      res.set_content("{\"error\":\"invalid_amount\"}", "application/json");
      return;
    }

    int64_t balance_after = 0;
    const string user_id = std::to_string(*uid);
    const auto user_before = storage->GetUserById(user_id);
    if (!user_before.has_value()) {
      std::cerr << "[economy_action] action=borrow"
                << " user_id=" << user_id
                << " amount=" << *amount
                << " profile=" << runtime_cfg.persistence_profile
                << " intent_type=UserBalanceChanged+TreasuryBalanceChanged"
                << " policy_result=validation_reject"
                << " rejection_reason=unauthorized"
                << " treasury_balance_before=-1\n";
      res.status = 404;
      res.set_content("{\"error\":\"user_not_found\"}", "application/json");
      return;
    }
    const auto eco_before = economy.GetState();
    const string period_key = eco_before.period_id;
    if (eco_before.global.treasury_balance < *amount) {
      std::cerr << "[economy_action] action=borrow"
                << " user_id=" << user_id
                << " amount=" << *amount
                << " profile=" << runtime_cfg.persistence_profile
                << " intent_type=UserBalanceChanged+TreasuryBalanceChanged"
                << " policy_result=validation_reject"
                << " rejection_reason=insufficient_treasury"
                << " treasury_balance_before=" << eco_before.global.treasury_balance << "\n";
      res.status = 409;
      res.set_content("{\"error\":\"insufficient_treasury\"}", "application/json");
      return;
    }
    std::string borrow_error;
    if (!storage->BorrowCellsAndTrackPeriod(user_id, *amount, period_key, balance_after, &borrow_error)) {
      if (borrow_error.empty()) borrow_error = "internal_error";
      const std::string rejection_reason = (borrow_error == "policy_rejected")
                                               ? "persistence_write_failed"
                                               : borrow_error;
      std::cerr << "[borrow] reject user_id=" << user_id
                << " amount=" << *amount
                << " user_liquid_before=" << (user_before.has_value() ? user_before->balance_mi : -1)
                << " treasury_before=" << eco_before.global.treasury_balance
                << " money_supply_before=" << eco_before.global.m
                << " reason=" << borrow_error << "\n";
      std::cerr << "[economy_action] action=borrow"
                << " user_id=" << user_id
                << " amount=" << *amount
                << " profile=" << runtime_cfg.persistence_profile
                << " intent_type=UserBalanceChanged+TreasuryBalanceChanged"
                << " policy_result=storage_reject"
                << " rejection_reason=" << rejection_reason
                << " treasury_balance_before=" << eco_before.global.treasury_balance << "\n";
      res.status = (borrow_error == "internal_error") ? 500 : 409;
      res.set_content("{\"error\":\"" + json_escape(borrow_error) + "\"}", "application/json");
      return;
    }

    economy.InvalidateCache();
    const auto eco = economy.GetState();
    const auto user_after = storage->GetUserById(user_id);
    std::cerr << "[borrow] success user_id=" << user_id
              << " amount=" << *amount
              << " user_liquid_before=" << (user_before.has_value() ? user_before->balance_mi : -1)
              << " user_liquid_after=" << (user_after.has_value() ? user_after->balance_mi : balance_after)
              << " treasury_before=" << eco_before.global.treasury_balance
              << " treasury_after=" << eco.global.treasury_balance
              << " money_supply_before=" << eco_before.global.m
              << " money_supply_after=" << eco.global.m << "\n";
    std::cerr << "[economy_action] action=borrow"
              << " user_id=" << user_id
              << " amount=" << *amount
              << " profile=" << runtime_cfg.persistence_profile
              << " intent_type=UserBalanceChanged+TreasuryBalanceChanged"
              << " policy_result=applied"
              << " rejection_reason=none"
              << " treasury_balance_before=" << eco_before.global.treasury_balance << "\n";
    maybe_resize_world_from_economy(eco);
    const int64_t a_world = economy_world_area(eco.params, eco.global);
    const int64_t m_white = std::max<int64_t>(0, a_world - eco.global.k);

    ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"amount\":" << *amount << ","
      << "\"balance_mi\":" << balance_after << ","
      << "\"liquid_assets\":" << balance_after << ","
      << "\"period_key\":\"" << json_escape(eco.period_id) << "\","
      << "\"economy\":{"
      << "\"M\":" << eco.global.m << ","
      << "\"K\":" << eco.global.k << ","
      << "\"A_world\":" << a_world << ","
      << "\"M_white\":" << m_white << ","
      << "\"extracted_output\":" << eco.global.y << ","
      << "\"P\":" << json_number(eco.global.p) << ","
      << "\"pi\":" << json_number(eco.global.pi)
      << "}"
      << "}";
    res.set_content(o.str(), "application/json");
  };

  srv.Post("/user/borrow", handle_borrow_cells);
  srv.Post("/economy/purchase", handle_borrow_cells);

  srv.Post(R"(/snake/(\d+)/attach)", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    res.set_header("X-Snake-Attach-Api", "v2");
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    int snake_id = stoi(req.matches[1]);
    auto amount = get_json_int_field(req.body, "amount");
    if (!amount || *amount <= 0) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_amount\"}", "application/json");
      return;
    }

    const std::string uid_str = std::to_string(*uid);
    const std::string snake_id_str = std::to_string(snake_id);
    const auto eco_before = economy.GetState();
    const auto user = storage->GetUserById(uid_str);
    if (!user.has_value()) {
      res.status = 404;
      res.set_content("{\"error\":\"user_not_found\"}", "application/json");
      return;
    }
    if (user->balance_mi < *amount) {
      res.status = 409;
      res.set_content("{\"error\":\"insufficient_cells\"}", "application/json");
      return;
    }
    const auto snake = storage->GetSnakeById(snake_id_str);
    if (!snake.has_value()) {
      res.status = 404;
      std::ostringstream visible_ids;
      bool first = true;
      for (const auto& s : storage->ListSnakes()) {
        if (s.owner_user_id != uid_str) continue;
        if (!first) visible_ids << ",";
        first = false;
        visible_ids << s.snake_id;
      }
      std::cerr << "[economy_action] action=attach"
                << " user_id=" << uid_str
                << " snake_id_requested=" << snake_id_str
                << " snake_ids_visible_to_user=" << visible_ids.str()
                << " profile=" << runtime_cfg.persistence_profile
                << " read_layer_path_used=durable_storage"
                << " write_layer_path_used_for_snake_creation=none"
                << " attach_lookup_result=snake_not_found\n";
      std::cerr << "[attach_trace] action=attach route=/snake/{id}/attach"
                << " user_id=" << uid_str
                << " snake_id_requested=" << snake_id_str
                << " amount=" << *amount
                << " profile=" << runtime_cfg.persistence_profile
                << " lookup_source=durable_storage"
                << " snake_lookup_succeeded=false"
                << " create_snake_invoked=false"
                << " response_code=404"
                << " error=snake_not_found\n";
      res.set_content("{\"error\":\"snake_not_found\"}", "application/json");
      return;
    }
    if (!snake->alive || !snake->is_on_field || snake->length_k <= 0) {
      res.status = 409;
      res.set_content("{\"error\":\"snake_not_attachable\"}", "application/json");
      return;
    }
    if (snake->owner_user_id != uid_str) {
      res.status = 403;
      res.set_content("{\"error\":\"forbidden\"}", "application/json");
      return;
    }
    const std::string resolved_snake_id_str = std::to_string(snake_id);

    int64_t balance_after = 0;
    int64_t snake_len_after = 0;
    if (!storage->AttachCellsToSnake(uid_str, resolved_snake_id_str, *amount, balance_after, snake_len_after)) {
      res.status = 409;
      std::cerr << "[attach_trace] action=attach route=/snake/{id}/attach"
                << " user_id=" << uid_str
                << " snake_id_requested=" << snake_id_str
                << " amount=" << *amount
                << " profile=" << runtime_cfg.persistence_profile
                << " lookup_source=durable_storage"
                << " snake_lookup_succeeded=true"
                << " create_snake_invoked=false"
                << " response_code=409"
                << " error=attach_conflict\n";
      res.set_content("{\"error\":\"attach_conflict\"}", "application/json");
      return;
    }

    auto world_len_after = game.attach_cells_to_snake(*uid, snake_id, *amount);
    if (!world_len_after.has_value()) {
      // Keep DB and runtime eventually consistent if this process missed the snake.
      game.load_from_storage_or_seed_positions();
      const auto refreshed = game.attach_cells_to_snake(*uid, snake_id, *amount);
      if (!refreshed.has_value()) {
        res.status = 500;
        res.set_content("{\"error\":\"attach_runtime_failed\"}", "application/json");
        return;
      }
      world_len_after = refreshed;
    }
    game.flush_persistence_delta();
    economy.InvalidateCache();
    const auto eco_after = economy.GetState();
    std::cerr << "[attach] success user_id=" << uid_str
              << " snake_id=" << resolved_snake_id_str
              << " amount=" << *amount
              << " user_liquid_before=" << user->balance_mi
              << " user_liquid_after=" << balance_after
              << " snake_length_before=" << snake->length_k
              << " snake_length_after=" << *world_len_after
              << " total_capital_before=" << eco_before.global.k
              << " total_capital_after=" << eco_after.global.k
              << " free_space_before=" << eco_before.stabilization.free_space_on_field
              << " free_space_after=" << eco_after.stabilization.free_space_on_field
              << " money_supply_before=" << eco_before.global.m
              << " money_supply_after=" << eco_after.global.m << "\n";
    std::cerr << "[economy_action] action=attach"
              << " user_id=" << uid_str
              << " snake_id_requested=" << snake_id_str
              << " snake_ids_visible_to_user=" << resolved_snake_id_str
              << " profile=" << runtime_cfg.persistence_profile
              << " read_layer_path_used=durable_storage"
              << " write_layer_path_used_for_snake_creation=none"
              << " attach_lookup_result=resolved\n";
    std::cerr << "[attach_trace] action=attach route=/snake/{id}/attach"
              << " user_id=" << uid_str
              << " snake_id_requested=" << snake_id_str
              << " amount=" << *amount
              << " profile=" << runtime_cfg.persistence_profile
              << " lookup_source=durable_storage"
              << " snake_lookup_succeeded=true"
              << " create_snake_invoked=false"
              << " response_code=200"
              << " error=none\n";

    ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"balance_mi\":" << balance_after << ","
      << "\"liquid_assets\":" << balance_after << ","
      << "\"snake_length_k\":" << *world_len_after
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post("/game/camera", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized_camera\"}", "application/json");
      return;
    }
    auto sid = get_json_string_field(req.body, "sid");
    auto x = get_json_int_field(req.body, "x");
    auto y = get_json_int_field(req.body, "y");
    if (!sid || sid->empty() || !x || !y) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_camera_payload\"}", "application/json");
      return;
    }

    ClientSession session = get_or_create_session(*sid);
    const uint64_t now_millis = now_ms();
    const uint64_t min_gap = static_cast<uint64_t>(max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(runtime_cfg.camera_msg_max_hz)))));
    if (session.last_camera_update_ms != 0 && now_millis - session.last_camera_update_ms < min_gap) {
      res.set_content("{\"status\":\"THROTTLED\"}", "application/json");
      return;
    }
    session.camera_x = max(0, min(grid_w - 1, *x));
    session.camera_y = max(0, min(grid_h - 1, *y));
    auto zoom = get_json_double_field(req.body, "zoom");
    if (zoom.has_value()) {
      session.camera_zoom = max(0.25, min(4.0, *zoom));
    }
    session.auth_user_id = *uid;
    session.subscribed_chunks_count = compute_subscribed_chunks_count(session);
    session.last_camera_update_ms = now_millis;
    session.updated_at_ms = now_ms();

    auto watch_snake = get_json_int_field(req.body, "watch_snake_id");
    if (watch_snake && *watch_snake > 0) {
      session.watched_snake_id = *watch_snake;
    } else {
      session.watched_snake_id.reset();
    }

    {
      lock_guard<mutex> lock(sessions_mu);
      sessions[*sid] = session;
    }
    ostringstream out;
    out << "{"
        << "\"status\":\"OK\","
        << "\"camera_x\":" << session.camera_x << ","
        << "\"camera_y\":" << session.camera_y << ","
        << "\"camera_zoom\":" << json_number(session.camera_zoom) << ","
        << "\"aoi_chunks\":" << session.subscribed_chunks_count << ","
        << "\"mode\":\"AUTH\","
        << "\"aoi_enabled\":" << (runtime_cfg.aoi_enabled ? "true" : "false")
        << "}";
    res.set_content(out.str(), "application/json");
  });

  srv.Get("/game/view", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    const string sid = (req.has_param("sid") && !req.get_param_value("sid").empty())
                           ? req.get_param_value("sid")
                           : rand_token(16);
    auto session = get_or_create_session(sid);

    optional<int> uid;
    if (req.has_param("token")) {
      const auto token = req.get_param_value("token");
      if (!token.empty()) uid = auth.token_to_user(token);
    }
    if (!uid) uid = require_auth_user(auth, req);

    if (uid) {
      session.auth_user_id = *uid;
      {
        lock_guard<mutex> lock(sessions_mu);
        sessions[sid] = session;
      }
      ostringstream o;
      o << "{"
        << "\"mode\":\"AUTH\","
        << "\"camera_x\":" << session.camera_x << ","
        << "\"camera_y\":" << session.camera_y << ","
        << "\"zoom\":" << json_number(session.camera_zoom) << ","
        << "\"aoi_radius\":" << runtime_cfg.auth_aoi_radius << ","
        << "\"aoi_pad_chunks\":" << runtime_cfg.aoi_pad_chunks << ","
        << "\"aoi_chunks\":" << session.subscribed_chunks_count
        << "}";
      res.set_content(o.str(), "application/json");
      return;
    }

    PublicViewState pv;
    {
      lock_guard<mutex> lock(public_view_mu);
      pv = public_view;
    }
    ostringstream o;
    o << "{"
      << "\"mode\":\"PUBLIC\","
      << "\"camera_x\":" << pv.camera_x << ","
      << "\"camera_y\":" << pv.camera_y << ","
      << "\"zoom\":1.0,"
      << "\"aoi_radius\":" << runtime_cfg.public_aoi_radius << ","
      << "\"aoi_pad_chunks\":" << runtime_cfg.aoi_pad_chunks << ","
      << "\"aoi_chunks\":" << (runtime_cfg.single_chunk_mode ? 1 : ((runtime_cfg.public_aoi_radius + runtime_cfg.aoi_pad_chunks) * 2 + 1) * ((runtime_cfg.public_aoi_radius + runtime_cfg.aoi_pad_chunks) * 2 + 1)) << ","
      << "\"public_camera_chunk\":{\"cx\":" << pv.chunk_cx << ",\"cy\":" << pv.chunk_cy << "}"
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/game/stream", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Content-Type", "text/event-stream");

    const string sid = (req.has_param("sid") && !req.get_param_value("sid").empty())
                           ? req.get_param_value("sid")
                           : rand_token(16);
    ClientSession initial_session = get_or_create_session(sid);
    optional<int> stream_uid;
    if (req.has_param("token")) {
      const auto token = req.get_param_value("token");
      if (!token.empty()) stream_uid = auth.token_to_user(token);
    }
    if (!stream_uid) stream_uid = require_auth_user(auth, req);
    if (stream_uid) {
      initial_session.auth_user_id = *stream_uid;
      initial_session.updated_at_ms = now_ms();
      lock_guard<mutex> lock(sessions_mu);
      sessions[sid] = initial_session;
    }

    res.set_chunked_content_provider(
        "text/event-stream",
        [&](size_t, httplib::DataSink& sink) {
          uint64_t last_seq = 0;
          auto last_heartbeat = chrono::steady_clock::now();
          const auto heartbeat_every = chrono::seconds(10);
          auto next_send_at = chrono::steady_clock::now();
          while (true) {
            string payload;
            uint64_t current_seq = 0;
            {
              lock_guard<mutex> lock(snapshot_mu);
              current_seq = snapshot_seq;
            }
            if (current_seq != last_seq) {
              ClientSession session = get_or_create_session(sid);
              const int rate_hz = runtime_cfg.spectator_hz;
              const auto send_dt = chrono::milliseconds(
                  max(1, static_cast<int>(std::lround(1000.0 / static_cast<double>(rate_hz)))));
              const auto now = chrono::steady_clock::now();
              if (now >= next_send_at) {
                last_seq = current_seq;
                next_send_at = now + send_dt;
                session.subscribed_chunks_count = -1;
                {
                  lock_guard<mutex> lock(sessions_mu);
                  sessions[sid] = session;
                }

                const string encoded = state_to_json(game.snapshot());
                payload = "event: frame\n";
                payload += "data: " + encoded + "\n\n";
              }
            }
            if (payload.empty()) {
              const auto now = chrono::steady_clock::now();
              if (now - last_heartbeat >= heartbeat_every) {
                payload = ": keepalive\n\n";
                last_heartbeat = now;
              }
            }
            if (!payload.empty() && !sink.write(payload.data(), payload.size())) break;
            const int poll_ms = max(1, runtime_cfg.SpectatorIntervalMs() / 2);
            this_thread::sleep_for(chrono::milliseconds(poll_ms));
          }
          sink.done();
          return true;
        });
  });

  auto next_numeric_user_id = [&]() -> int {
    int mx = 0;
    for (const auto& u : storage->ListUsers()) {
      try {
        mx = std::max(mx, std::stoi(u.user_id));
      } catch (...) {
      }
    }
    return mx + 1;
  };

  srv.Post("/auth/login", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (runtime_cfg.google_auth_enabled) {
      res.status = 403;
      res.set_content("{\"error\":\"password_login_disabled\"}", "application/json");
      return;
    }
    auto u = get_json_string_field(req.body, "username");
    auto p = get_json_string_field(req.body, "password");
    if (!u || !p) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_request\"}", "application/json");
      return;
    }

    string token;
    auto uid = user_login(*storage, auth, *u, *p, token);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    ostringstream o;
    o << "{\"token\":\"" << json_escape(token) << "\",\"user_id\":" << *uid << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post("/auth/google", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!runtime_cfg.google_auth_enabled) {
      res.status = 403;
      res.set_content("{\"error\":\"google_auth_disabled\"}", "application/json");
      return;
    }
    if (runtime_cfg.google_client_id.empty()) {
      res.status = 500;
      res.set_content("{\"error\":\"google_client_id_missing\"}", "application/json");
      return;
    }
    auto id_token = get_json_string_field(req.body, "id_token");
    if (!id_token || id_token->empty()) {
      res.status = 400;
      res.set_content("{\"error\":\"missing_id_token\"}", "application/json");
      return;
    }
    const auto claims = verify_google_id_token_with_google(*id_token);
    if (!claims.has_value()) {
      res.status = 401;
      res.set_content("{\"error\":\"invalid_google_token\"}", "application/json");
      return;
    }
    if (!is_google_issuer_allowed(claims->issuer)) {
      res.status = 401;
      res.set_content("{\"error\":\"invalid_google_issuer\"}", "application/json");
      return;
    }
    if (claims->audience != runtime_cfg.google_client_id) {
      res.status = 401;
      res.set_content("{\"error\":\"invalid_google_audience\"}", "application/json");
      return;
    }
    const int64_t now_s = static_cast<int64_t>(std::time(nullptr));
    if (claims->exp < now_s - 30) {
      res.status = 401;
      res.set_content("{\"error\":\"google_token_expired\"}", "application/json");
      return;
    }

    auto user = storage->GetUserByGoogleSubject(claims->subject);
    bool first_login = false;
    if (!user.has_value()) {
      first_login = true;
      storage::User nu;
      nu.user_id = std::to_string(next_numeric_user_id());
      nu.username = "g_" + claims->subject.substr(0, std::min<size_t>(claims->subject.size(), 20));
      nu.password_hash = "";
      nu.balance_mi = 0;
      nu.created_at = now_s;
      nu.updated_at = now_s;
      nu.auth_provider = "google";
      nu.google_subject_id = claims->subject;
      nu.onboarding_completed = false;
      nu.account_status = "active";
      if (!storage->PutUser(nu)) {
        res.status = 500;
        res.set_content("{\"error\":\"user_create_failed\"}", "application/json");
        return;
      }
      user = storage->GetUserById(nu.user_id);
    }
    if (!user.has_value()) {
      res.status = 500;
      res.set_content("{\"error\":\"user_load_failed\"}", "application/json");
      return;
    }
    if (user->account_status == "deleted") {
      res.status = 403;
      res.set_content("{\"error\":\"account_deleted\"}", "application/json");
      return;
    }
    if (user->auth_provider != "google") {
      user->auth_provider = "google";
      user->updated_at = now_s;
      (void)storage->PutUser(*user);
    }
    int uid = 0;
    try {
      uid = stoi(user->user_id);
    } catch (...) {
      res.status = 500;
      res.set_content("{\"error\":\"user_id_invalid\"}", "application/json");
      return;
    }
    string token = auth.issue_token(uid);
    ostringstream o;
    o << "{"
      << "\"token\":\"" << json_escape(token) << "\","
      << "\"user_id\":" << uid << ","
      << "\"first_login\":" << (first_login ? "true" : "false") << ","
      << "\"onboarding_required\":" << (user->onboarding_completed ? "false" : "true")
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/names/check-company", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!req.has_param("name")) {
      res.status = 400;
      res.set_content("{\"error\":\"missing_name\"}", "application/json");
      return;
    }
    const string name = req.get_param_value("name");
    const bool valid = is_valid_game_name(name);
    const string norm = normalize_name(name);
    const bool taken = valid ? storage->CompanyNameExistsNormalized(norm) : false;
    ostringstream o;
    o << "{\"valid\":" << (valid ? "true" : "false")
      << ",\"taken\":" << (taken ? "true" : "false") << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/names/check-snake", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    if (!req.has_param("name")) {
      res.status = 400;
      res.set_content("{\"error\":\"missing_name\"}", "application/json");
      return;
    }
    const string name = req.get_param_value("name");
    const bool valid = is_valid_game_name(name);
    const string norm = normalize_name(name);
    const bool taken = valid ? storage->SnakeNameExistsNormalized(norm) : false;
    ostringstream o;
    o << "{\"valid\":" << (valid ? "true" : "false")
      << ",\"taken\":" << (taken ? "true" : "false") << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post("/onboarding/complete", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid_opt = require_auth_user(auth, req);
    if (!uid_opt) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }
    const int uid = *uid_opt;
    const string user_id = std::to_string(uid);
    auto user = storage->GetUserById(user_id);
    if (!user.has_value()) {
      res.status = 404;
      res.set_content("{\"error\":\"user_not_found\"}", "application/json");
      return;
    }
    if (user->onboarding_completed) {
      res.set_content("{\"ok\":true,\"already_completed\":true}", "application/json");
      return;
    }
    auto company_name = get_json_string_field(req.body, "company_name");
    auto snake_name = get_json_string_field(req.body, "snake_name");
    if (!company_name || !snake_name || !is_valid_game_name(*company_name) || !is_valid_game_name(*snake_name)) {
      res.status = 400;
      res.set_content("{\"error\":\"invalid_name\"}", "application/json");
      return;
    }
    const string company_norm = normalize_name(*company_name);
    const string snake_norm = normalize_name(*snake_name);
    if (storage->CompanyNameExistsNormalized(company_norm, user_id)) {
      res.status = 409;
      res.set_content("{\"error\":\"company_name_taken\"}", "application/json");
      return;
    }
    if (storage->SnakeNameExistsNormalized(snake_norm)) {
      res.status = 409;
      res.set_content("{\"error\":\"snake_name_taken\"}", "application/json");
      return;
    }

    int starter_snake_id = 0;
    bool starter_snake_created_now = false;
    auto existing = game.list_user_snakes(uid);
    if (!existing.empty()) {
      starter_snake_id = existing.front().id;
    } else {
      const string color = "#00ffaa";
      auto created = game.create_snake_for_user(uid, color);
      if (!created.has_value()) {
        res.status = 500;
        res.set_content("{\"error\":\"starter_snake_create_failed\"}", "application/json");
        return;
      }
      starter_snake_id = *created;
      starter_snake_created_now = true;
      game.flush_persistence_delta();
      persistence_coordinator.FlushNow();
    }

    auto starter_snake = storage->GetSnakeById(std::to_string(starter_snake_id));
    if (starter_snake.has_value()) {
      starter_snake->snake_name = *snake_name;
      starter_snake->snake_name_normalized = snake_norm;
      starter_snake->updated_at = static_cast<int64_t>(time(nullptr));
      if (!storage->PutSnake(*starter_snake)) {
        if (storage->SnakeNameExistsNormalized(snake_norm, std::to_string(starter_snake_id))) {
          // Best-effort rollback for a starter snake created in this request.
          if (starter_snake_created_now) {
            (void)storage->DeleteSnake(std::to_string(starter_snake_id));
            game.load_from_storage_or_seed_positions();
          }
          res.status = 409;
          res.set_content("{\"error\":\"snake_name_taken\"}", "application/json");
          return;
        }
        res.status = 500;
        res.set_content("{\"error\":\"starter_snake_update_failed\"}", "application/json");
        return;
      }
    }

    // Guard starter grants so retried onboarding requests do not double-credit assets.
    const bool should_grant_starter_assets =
        runtime_cfg.starter_liquid_assets > 0 &&
        user->starter_snake_id.empty() &&
        user->balance_mi < runtime_cfg.starter_liquid_assets;
    if (should_grant_starter_assets) {
      if (!storage->IncrementUserBalance(user_id, runtime_cfg.starter_liquid_assets)) {
        res.status = 500;
        res.set_content("{\"error\":\"starter_assets_failed\"}", "application/json");
        return;
      }
    }

    user = storage->GetUserById(user_id);
    if (!user.has_value()) {
      res.status = 500;
      res.set_content("{\"error\":\"user_reload_failed\"}", "application/json");
      return;
    }
    user->company_name = *company_name;
    user->company_name_normalized = company_norm;
    user->onboarding_completed = true;
    user->starter_snake_id = std::to_string(starter_snake_id);
    user->updated_at = static_cast<int64_t>(time(nullptr));
    if (!storage->PutUser(*user)) {
      res.status = 500;
      res.set_content("{\"error\":\"user_update_failed\"}", "application/json");
      return;
    }
    ostringstream o;
    o << "{\"ok\":true,\"starter_snake_id\":" << starter_snake_id
      << ",\"starter_liquid_assets\":" << runtime_cfg.starter_liquid_assets << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post("/settings/delete-account", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid_opt = require_auth_user(auth, req);
    if (!uid_opt) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }
    const int uid = *uid_opt;
    const string user_id = std::to_string(uid);
    auto user = storage->GetUserById(user_id);
    if (!user.has_value()) {
      res.status = 404;
      res.set_content("{\"error\":\"user_not_found\"}", "application/json");
      return;
    }
    auto confirm = get_json_string_field(req.body, "company_name_confirmation");
    if (!confirm || *confirm != user->company_name) {
      res.status = 400;
      res.set_content("{\"error\":\"company_name_mismatch\"}", "application/json");
      return;
    }
    const auto all_snakes = storage->ListSnakes();
    for (const auto& s : all_snakes) {
      if (s.owner_user_id != user_id) continue;
      (void)storage->DeleteSnakeEventsBySnakeId(s.snake_id);
      (void)storage->DeleteSnake(s.snake_id);
    }
    if (!storage->DeleteUserById(user_id)) {
      res.status = 500;
      res.set_content("{\"error\":\"delete_failed\"}", "application/json");
      return;
    }
    std::cerr << "[account_delete] user_id=" << user_id << " company_name=" << user->company_name << "\n";
    game.load_from_storage_or_seed_positions();
    res.set_content("{\"ok\":true}", "application/json");
  });

  auto build_owned_snake_view = [&](int uid,
                                    std::string* out_reason = nullptr) -> std::vector<storage::Snake> {
    std::unordered_map<int, storage::Snake> merged_by_id;
    const std::string uid_str = std::to_string(uid);
    if (out_reason) out_reason->clear();

    try {
      const auto runtime_snakes = game.list_user_snakes(uid);
      for (const auto& rs : runtime_snakes) {
        storage::Snake s;
        s.snake_id = std::to_string(rs.id);
        s.owner_user_id = uid_str;
        s.color = rs.color;
        s.paused = rs.paused;
        s.length_k = static_cast<int>(rs.body.size());
        merged_by_id[rs.id] = s;
      }
    } catch (const std::exception& ex) {
      std::cerr << "[me_snakes] user_id=" << uid_str
                << " reason=owned_snake_query_failed"
                << " layer=runtime"
                << " error=" << ex.what() << "\n";
      if (out_reason) *out_reason = "owned_snake_query_failed";
    } catch (...) {
      std::cerr << "[me_snakes] user_id=" << uid_str
                << " reason=owned_snake_query_failed"
                << " layer=runtime"
                << " error=unknown\n";
      if (out_reason) *out_reason = "owned_snake_query_failed";
    }

    try {
      for (const auto& s : storage->ListSnakes()) {
        if (s.owner_user_id != uid_str) continue;
        int snake_id_num = 0;
        try {
          snake_id_num = std::stoi(s.snake_id);
        } catch (...) {
          std::cerr << "[me_snakes] user_id=" << uid_str
                    << " reason=owned_snake_hydration_failed"
                    << " snake_id_raw=" << s.snake_id << "\n";
          continue;
        }
        auto it = merged_by_id.find(snake_id_num);
        if (it == merged_by_id.end()) {
          merged_by_id.emplace(snake_id_num, s);
        } else {
          // Prefer durable name metadata, keep runtime length when larger/newer.
          if (!s.snake_name.empty()) it->second.snake_name = s.snake_name;
          if (!s.snake_name_normalized.empty()) it->second.snake_name_normalized = s.snake_name_normalized;
          if (!s.color.empty()) it->second.color = s.color;
          it->second.paused = s.paused;
          it->second.length_k = std::max(it->second.length_k, s.length_k);
        }
      }
    } catch (const std::exception& ex) {
      std::cerr << "[me_snakes] user_id=" << uid_str
                << " reason=owned_snake_query_failed"
                << " layer=durable"
                << " error=" << ex.what() << "\n";
      if (out_reason) *out_reason = "owned_snake_query_failed";
    } catch (...) {
      std::cerr << "[me_snakes] user_id=" << uid_str
                << " reason=owned_snake_query_failed"
                << " layer=durable"
                << " error=unknown\n";
      if (out_reason) *out_reason = "owned_snake_query_failed";
    }

    std::vector<storage::Snake> snakes;
    snakes.reserve(merged_by_id.size());
    for (auto& kv : merged_by_id) snakes.push_back(std::move(kv.second));
    std::sort(snakes.begin(), snakes.end(), [](const storage::Snake& a, const storage::Snake& b) {
      try {
        return std::stoi(a.snake_id) < std::stoi(b.snake_id);
      } catch (...) {
        return a.snake_id < b.snake_id;
      }
    });
    return snakes;
  };

  srv.Get("/me/snakes", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    const auto snakes = build_owned_snake_view(*uid);
    ostringstream o;
    o << "{\"snakes\":[";
    bool first_written = true;
    for (size_t i = 0; i < snakes.size(); ++i) {
      int snake_id_num = 0;
      try {
        snake_id_num = std::stoi(snakes[i].snake_id);
      } catch (...) {
        continue;
      }
      const std::string snake_name =
          !snakes[i].snake_name.empty() ? snakes[i].snake_name : ("Snake #" + std::to_string(snake_id_num));
      if (!first_written) o << ",";
      first_written = false;
      o << "{" << "\"id\":" << snake_id_num << ","
        << "\"name\":\"" << json_escape(snake_name) << "\","
        << "\"color\":\"" << json_escape(snakes[i].color) << "\","
        << "\"paused\":" << (snakes[i].paused ? "true" : "false") << ","
        << "\"len\":" << snakes[i].length_k << "}";
    }
    o << "]}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post(R"(/snake/(\d+)/rename)", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }
    int snake_id = stoi(req.matches[1]);
    auto snake_name = get_json_string_field(req.body, "snake_name");
    if (!snake_name || !is_valid_game_name(*snake_name)) {
      res.status = 400;
      res.set_content("{\"error\":\"invalid_snake_name\"}", "application/json");
      return;
    }
    const std::string snake_name_norm = normalize_name(*snake_name);
    auto snake = storage->GetSnakeById(std::to_string(snake_id));
    if (!snake.has_value()) {
      res.status = 404;
      res.set_content("{\"error\":\"snake_not_found\"}", "application/json");
      return;
    }
    if (snake->owner_user_id != std::to_string(*uid)) {
      res.status = 403;
      res.set_content("{\"error\":\"forbidden\"}", "application/json");
      return;
    }
    if (storage->SnakeNameExistsNormalized(snake_name_norm, std::to_string(snake_id))) {
      res.status = 409;
      res.set_content("{\"error\":\"snake_name_taken\"}", "application/json");
      return;
    }
    snake->snake_name = *snake_name;
    snake->snake_name_normalized = snake_name_norm;
    snake->updated_at = static_cast<int64_t>(time(nullptr));
    if (!storage->PutSnake(*snake)) {
      if (storage->SnakeNameExistsNormalized(snake_name_norm, std::to_string(snake_id))) {
        res.status = 409;
        res.set_content("{\"error\":\"snake_name_taken\"}", "application/json");
        return;
      }
      res.status = 500;
      res.set_content("{\"error\":\"rename_failed\"}", "application/json");
      return;
    }
    std::ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"id\":" << snake_id << ","
      << "\"name\":\"" << json_escape(snake->snake_name) << "\""
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Post(R"(/snakes/(\d+)/dir)", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    int snake_id = stoi(req.matches[1]);
    auto d = get_json_int_field(req.body, "dir");
    if (!d || *d < 1 || *d > 4) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_dir\"}", "application/json");
      return;
    }

    bool ok = game.set_snake_dir(*uid, snake_id, static_cast<world::Dir>(*d));
    if (!ok) {
      res.status = 403;
      res.set_content("{\"error\":\"forbidden\"}", "application/json");
      return;
    }
    res.set_content("{\"status\":\"OK\"}", "application/json");
  });

  srv.Post(R"(/snakes/(\d+)/pause)", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    int snake_id = stoi(req.matches[1]);
    bool ok = game.toggle_snake_pause(*uid, snake_id);
    if (!ok) {
      res.status = 403;
      res.set_content("{\"error\":\"forbidden\"}", "application/json");
      return;
    }
    res.set_content("{\"status\":\"OK\"}", "application/json");
  });

  srv.Post("/me/snakes", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    auto c = get_json_string_field(req.body, "color");
    auto snake_name = get_json_string_field(req.body, "snake_name");
    string color = c ? *c : "#ff00ff";
    const string uid_str = std::to_string(*uid);
    if (!snake_name || !is_valid_game_name(*snake_name)) {
      res.status = 400;
      res.set_content("{\"error\":\"invalid_snake_name\"}", "application/json");
      return;
    }
    const string snake_name_norm = normalize_name(*snake_name);
    if (storage->SnakeNameExistsNormalized(snake_name_norm)) {
      res.status = 409;
      res.set_content("{\"error\":\"snake_name_taken\"}", "application/json");
      return;
    }

    auto user = storage->GetUserById(uid_str);
    if (!user.has_value()) {
      res.status = 404;
      res.set_content("{\"error\":\"user_not_found\"}", "application/json");
      return;
    }
    if (user->balance_mi < 1) {
      res.status = 409;
      res.set_content("{\"error\":\"insufficient_cells\"}", "application/json");
      return;
    }
    if (!storage->IncrementUserBalance(uid_str, -1)) {
      res.status = 500;
      res.set_content("{\"error\":\"balance_update_failed\"}", "application/json");
      return;
    }

    auto id = game.create_snake_for_user(*uid, color);
    if (!id) {
      // Roll back storage debit when snake creation fails (limit/placement/etc).
      storage->IncrementUserBalance(uid_str, 1);
      res.status = 429;
      res.set_content("{\"error\":\"snake_limit\"}", "application/json");
      return;
    }
    game.flush_persistence_delta();
    persistence_coordinator.FlushNow();
    std::string snake_view_reason;
    const auto visible_snakes = build_owned_snake_view(*uid, &snake_view_reason);
    bool visible_in_view = false;
    for (const auto& s : visible_snakes) {
      if (s.snake_id == std::to_string(*id)) {
        visible_in_view = true;
        break;
      }
    }
    if (!visible_in_view) {
      res.status = 500;
      std::cerr << "[me_snakes_create] user_id=" << uid_str
                << " snake_id=" << *id
                << " reason="
                << (snake_view_reason.empty() ? "owned_snake_visibility_failed" : snake_view_reason)
                << "\n";
      res.set_content("{\"error\":\"snake_visibility_failed\"}", "application/json");
      return;
    }
    auto created_snake = storage->GetSnakeById(std::to_string(*id));
    if (created_snake.has_value()) {
      created_snake->snake_name = *snake_name;
      created_snake->snake_name_normalized = snake_name_norm;
      created_snake->updated_at = static_cast<int64_t>(time(nullptr));
      if (!storage->PutSnake(*created_snake)) {
        if (storage->SnakeNameExistsNormalized(snake_name_norm, std::to_string(*id))) {
          // Roll back snake create on uniqueness conflict so callers can retry cleanly.
          (void)storage->DeleteSnake(std::to_string(*id));
          (void)storage->IncrementUserBalance(uid_str, 1);
          game.load_from_storage_or_seed_positions();
          economy.InvalidateCache();
          res.status = 409;
          res.set_content("{\"error\":\"snake_name_taken\"}", "application/json");
          return;
        }
        std::cerr << "[me_snakes_create] user_id=" << uid_str
                  << " snake_id=" << *id
                  << " reason=owned_snake_serialization_failed\n";
      }
    } else {
      std::cerr << "[me_snakes_create] user_id=" << uid_str
                << " snake_id=" << *id
                << " reason=owned_snake_query_failed\n";
    }
    economy.InvalidateCache();

    const auto user_after = storage->GetUserById(uid_str);
    const int64_t balance_after = user_after ? user_after->balance_mi : 0;

    ostringstream o;
    o << "{"
      << "\"id\":" << *id << ","
      << "\"name\":\"" << json_escape(*snake_name) << "\","
      << "\"balance_mi\":" << balance_after << ","
      << "\"liquid_assets\":" << balance_after
      << "}";
    res.set_content(o.str(), "application/json");
  });

  cout << "Server on http://" << bind_host << ":" << bind_port << "\n";
  cout << "WS:    GET /ws\n";
  cout << "State: GET /game/state\n";
  cout << "Login: POST /auth/login {username,password}\n";

  srv.listen(bind_host, bind_port);

  running.store(false);
  loop.join();
  persistence_coordinator.Stop();
  game.flush_persistence_delta();
  Aws::ShutdownAPI(aws_options);
  return 0;
}
