// snake_server.cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <aws/core/Aws.h>

#include "economy/economy_v1.h"
#include "httplib.h"
#include "protocol/encode_json.h"
#include "storage/storage_factory.h"
#include "world/world.h"
#include "../config/runtime_config.h"

using namespace std;

static constexpr int DEFAULT_W = 40;
static constexpr int DEFAULT_H = 20;
static constexpr int FOOD_COUNT = 1;

static volatile sig_atomic_t g_reload_requested = 0;

static void on_reload_signal(int) {
  g_reload_requested = 1;
}

static uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static string utc_period_key_yyyymmddhh() {
  time_t t = time(nullptr);
  tm tm_utc{};
#if defined(_WIN32)
  gmtime_s(&tm_utc, &t);
#else
  gmtime_r(&t, &tm_utc);
#endif
  char buf[16];
  if (strftime(buf, sizeof(buf), "%Y%m%d%H", &tm_utc) == 0) return "1970010100";
  return buf;
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

static string rand_token(size_t n = 32) {
  static const char* chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static thread_local mt19937 rng(static_cast<uint32_t>(random_device{}()));
  uniform_int_distribution<int> dist(0, static_cast<int>(strlen(chars)) - 1);
  string t;
  t.reserve(n);
  for (size_t i = 0; i < n; ++i) t.push_back(chars[dist(rng)]);
  return t;
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
  uint64_t updated_at_ms = 0;
};

struct PublicViewState {
  int camera_x = DEFAULT_W / 2;
  int camera_y = DEFAULT_H / 2;
  int chunk_cx = 0;
  int chunk_cy = 0;
  uint64_t last_switch_tick = 0;
};

class GameService {
 public:
  GameService(storage::IStorage& storage, int width, int height, int food_count, int max_snakes_per_user)
      : storage_(storage),
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

  // Writes only event-driven deltas. No per-tick checkpoint persistence.
  int64_t flush_persistence_delta_and_credit_food(int64_t food_reward_cells) {
    const auto delta = world_.DrainPersistenceDelta(static_cast<int64_t>(now_ms()));
    if (delta.empty()) return 0;

    int64_t credited_food_events = 0;
    if (food_reward_cells > 0) {
      for (const auto& e : delta.snake_events) {
        if (e.event_type != "FOOD_EATEN") continue;
        auto snake = storage_.GetSnakeById(e.snake_id);
        if (!snake.has_value() || snake->owner_user_id.empty()) continue;
        if (storage_.IncrementUserBalance(snake->owner_user_id, food_reward_cells)) {
          ++credited_food_events;
        }
      }
    }
    for (const auto& ud : delta.user_balance_deltas) {
      if (ud.first.empty() || ud.second == 0) continue;
      (void)storage_.IncrementUserBalance(ud.first, ud.second);
    }
    if (delta.system_balance_delta != 0) {
      (void)storage_.IncrementSystemReserve(delta.system_balance_delta);
    }

    for (const auto& s : delta.upsert_snakes) storage_.PutSnake(s);
    for (const auto& sid : delta.delete_snake_ids) storage_.DeleteSnake(sid);
    if (delta.upsert_world_chunk.has_value()) storage_.PutWorldChunk(*delta.upsert_world_chunk);
    for (const auto& e : delta.snake_events) storage_.AppendSnakeEvent(e);
    return credited_food_events;
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
  world::World world_;
  int aoi_pad_chunks_ = 0;
  chrono::steady_clock::time_point last_empty_reload_attempt_{};
};

class EconomyService {
 public:
  explicit EconomyService(storage::IStorage& storage)
      : storage_(storage),
        cache_ttl_ms_([] {
          const char* env = getenv("ECONOMY_CACHE_MS");
          if (!env) return 2000;
          const int parsed = atoi(env);
          return max(500, min(10000, parsed));
        }()) {}

  struct Snapshot {
    economy::EconomyState state;
    storage::EconomyParams params;
    int64_t delta_m_buy = 0;
    int64_t k_snakes = 0;
  };

  Snapshot GetState() {
    using clock = chrono::steady_clock;
    const auto now = clock::now();

    {
      lock_guard<mutex> lock(mu_);
      if (cache_valid_ && now < cache_expire_at_) return cache_;
    }

    Snapshot fresh = ComputeFresh();
    {
      lock_guard<mutex> lock(mu_);
      cache_ = fresh;
      cache_valid_ = true;
      cache_expire_at_ = now + chrono::milliseconds(cache_ttl_ms_);
      return cache_;
    }
  }

  Snapshot RecomputeAndPersist(const string& period_key) {
    Snapshot fresh = ComputeFresh(period_key);
    storage::EconomyPeriod period;
    period.period_key = period_key;
    period.delta_m_buy = fresh.delta_m_buy;
    period.computed_m = fresh.state.m;
    period.computed_k = fresh.state.k;
    period.computed_y = static_cast<int64_t>(fresh.state.y);
    period.computed_p = static_cast<int64_t>(fresh.state.p * 1000000.0);
    period.computed_pi = static_cast<int64_t>(fresh.state.pi * 1000000.0);
    period.computed_world_area = fresh.state.a_world;
    period.computed_white = fresh.state.m_white;
    period.computed_at = static_cast<int64_t>(time(nullptr));
    storage_.PutEconomyPeriod(period);
    return fresh;
  }

  void InvalidateCache() {
    lock_guard<mutex> lock(mu_);
    cache_valid_ = false;
  }

 private:
  Snapshot ComputeFresh(const string& period_key) {
    Snapshot out;
    out.params = storage_.GetEconomyParamsActive().value_or(storage::EconomyParams{});
    const auto period = storage_.GetEconomyPeriod(period_key);
    out.delta_m_buy = period ? period->delta_m_buy : 0;

    const auto users = storage_.ListUsers();
    int64_t sum_mi = 0;
    for (const auto& u : users) sum_mi += u.balance_mi;

    const auto snakes = storage_.ListSnakes();
    out.k_snakes = 0;
    for (const auto& s : snakes) {
      if (s.alive && s.is_on_field) out.k_snakes += max<int64_t>(0, s.length_k);
    }

    economy::EconomyInputs in;
    in.params = out.params;
    in.sum_mi = sum_mi;
    in.m_g = out.params.m_gov_reserve;
    in.delta_m_buy = out.delta_m_buy;
    in.delta_m_issue = out.params.delta_m_issue;
    in.cap_delta_m = out.params.cap_delta_m;
    in.k_snakes = out.k_snakes;
    in.delta_k_obs = out.params.delta_k_obs;
    out.state = economy::ComputeEconomyV1(in, period_key);
    return out;
  }

  Snapshot ComputeFresh() {
    return ComputeFresh(utc_period_key_yyyymmddhh());
  }

  storage::IStorage& storage_;
  int cache_ttl_ms_ = 2000;
  mutex mu_;
  bool cache_valid_ = false;
  chrono::steady_clock::time_point cache_expire_at_{};
  Snapshot cache_{};
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
  res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
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
    defaults.updated_at = static_cast<int64_t>(time(nullptr));
    defaults.updated_by = "bootstrap";
    if (!storage->PutEconomyParamsActiveAndVersioned(defaults, "bootstrap")) {
      cerr << "Failed to initialize active economy params\n";
      Aws::ShutdownAPI(aws_options);
      return 1;
    }
  }

  GameService game(*storage, grid_w, grid_h, FOOD_COUNT, max_snakes_per_user);
  game.configure_chunking(runtime_cfg.chunk_size, runtime_cfg.single_chunk_mode);
  game.set_duel_delay_ticks(runtime_cfg.tick_hz);
  game.set_aoi_pad_chunks(runtime_cfg.aoi_pad_chunks);
  game.configure_mask(runtime_cfg.world_mask_mode, runtime_cfg.world_mask_seed, runtime_cfg.world_mask_style);
  EconomyService economy(*storage);
  game.load_from_storage_or_seed_positions();
  {
    const auto eco = economy.GetState();
    game.set_playable_cell_target(std::max<int64_t>(100, eco.state.a_world));
  }
  game.flush_persistence_delta();

  auto maybe_resize_world_from_economy = [&](const EconomyService::Snapshot& eco) {
    const auto current = game.snapshot();
    const int64_t current_area = static_cast<int64_t>(current.w) * static_cast<int64_t>(current.h);
    const int64_t target_area = max<int64_t>(100, eco.state.a_world);
    game.set_playable_cell_target(target_area);
    if (current_area <= 0) return;
    const double rel_diff = std::abs(static_cast<double>(target_area - current_area) / static_cast<double>(current_area));
    if (rel_diff < runtime_cfg.resize_threshold) return;
    const auto [new_w, new_h] = dims_from_area(target_area, runtime_cfg.world_aspect_ratio);
    game.resize_world(new_w, new_h);
    game.set_playable_cell_target(target_area);
    game.flush_persistence_delta();
  };

  if (mode == "reset") {
    if (!storage->ResetForDev()) {
      cerr << "Dynamo reset failed\n";
      Aws::ShutdownAPI(aws_options);
      return 1;
    }
    cout << "DynamoDB reset complete.\n";
    Aws::ShutdownAPI(aws_options);
    return 0;
  }
  if (mode == "seed") {
    seed(*storage, game);
    Aws::ShutdownAPI(aws_options);
    return 0;
  }
  if (mode != "serve") {
    cerr << "Usage: ./snake_server [serve|seed|reset]\n";
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
        const auto food_credits = game.flush_persistence_delta_and_credit_food(runtime_cfg.food_reward_cells);
        if (food_credits > 0) economy.InvalidateCache();
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
      }
      alive.store(false);
    });

    auto next_world_send = chrono::steady_clock::now();
    auto next_economy_send = chrono::steady_clock::now();
    auto next_private_send = chrono::steady_clock::now();

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
        ostringstream out;
        out << "{"
            << "\"type\":\"economy_world\","
            << "\"channel\":\"public\","
            << "\"M\":" << eco.state.m << ","
            << "\"P\":" << json_number(eco.state.p) << ","
            << "\"pi\":" << json_number(eco.state.pi) << ","
            << "\"A_world\":" << eco.state.a_world << ","
            << "\"M_white\":" << eco.state.m_white
            << "}";
        if (!ws.send(out.str())) break;
        next_economy_send = now + chrono::seconds(1);
      }

      if (is_auth && now >= next_private_send) {
        const auto user = storage->GetUserById(std::to_string(*session.auth_user_id));
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
              << "\"deployed_k\":" << deployed << ","
              << "\"snake_count\":" << snakes.size()
              << "}";
          if (!ws.send(out.str())) break;
        }
        next_private_send = now + chrono::seconds(5);
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
      << "\"aoi_enabled\":" << (runtime_cfg.aoi_enabled ? "true" : "false")
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/health", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    res.set_content("{\"ok\":true}", "application/json");
  });

  srv.Get("/economy/state", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    EconomyService::Snapshot s;
    try {
      s = economy.GetState();
    } catch (...) {
      // Endpoint must remain stable even when backing reads fail.
      s.params = storage::EconomyParams{};
      s.delta_m_buy = 0;
      s.k_snakes = 0;
      economy::EconomyInputs in;
      in.params = s.params;
      in.sum_mi = 0;
      in.m_g = s.params.m_gov_reserve;
      in.delta_m_buy = 0;
      in.delta_m_issue = s.params.delta_m_issue;
      in.cap_delta_m = s.params.cap_delta_m;
      in.k_snakes = 0;
      in.delta_k_obs = s.params.delta_k_obs;
      s.state = economy::ComputeEconomyV1(in, utc_period_key_yyyymmddhh());
    }
    ostringstream o;
    o << "{"
      << "\"period_key\":\"" << json_escape(s.state.period_key) << "\","
      << "\"M\":" << s.state.m << ","
      << "\"K\":" << s.state.k << ","
      << "\"Y\":" << json_number(s.state.y) << ","
      << "\"P\":" << json_number(s.state.p) << ","
      << "\"pi\":" << json_number(s.state.pi) << ","
      << "\"A_world\":" << s.state.a_world << ","
      << "\"M_white\":" << s.state.m_white << ","
      << "\"inputs\":{"
      << "\"k_land\":" << s.params.k_land << ","
      << "\"A\":" << json_number(s.params.a_productivity) << ","
      << "\"V\":" << json_number(s.params.v_velocity) << ","
      << "\"M_G\":" << s.params.m_gov_reserve << ","
      << "\"cap_delta_m\":" << s.params.cap_delta_m << ","
      << "\"delta_m_issue\":" << s.params.delta_m_issue << ","
      << "\"delta_m_buy\":" << s.delta_m_buy << ","
      << "\"delta_k_obs\":" << s.params.delta_k_obs << ","
      << "\"sum_mi\":" << s.state.sum_mi << ","
      << "\"k_snakes\":" << s.k_snakes
      << "}"
      << "}";
    res.set_content(o.str(), "application/json");
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

    const auto snakes = game.list_user_snakes(*uid);
    int64_t deployed = 0;
    for (const auto& s : snakes) deployed += static_cast<int64_t>(s.body.size());

    ostringstream o;
    o << "{"
      << "\"user_id\":" << *uid << ","
      << "\"balance_mi\":" << user->balance_mi << ","
      << "\"deployed_k\":" << deployed << ","
      << "\"snake_count\":" << snakes.size()
      << "}";
    res.set_content(o.str(), "application/json");
  });

  auto handle_borrow_cells = [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    auto amount = get_json_int_field(req.body, "amount");
    if (!amount) amount = get_json_int_field(req.body, "cells");
    if (!amount || *amount <= 0 || *amount > runtime_cfg.max_borrow_per_call) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_amount\"}", "application/json");
      return;
    }

    int64_t balance_after = 0;
    const string user_id = std::to_string(*uid);
    const string period_key = utc_period_key_yyyymmddhh();
    if (!storage->BorrowCellsAndTrackPeriod(user_id, *amount, period_key, balance_after)) {
      res.status = 500;
      res.set_content("{\"error\":\"borrow_failed\"}", "application/json");
      return;
    }

    economy.InvalidateCache();
    const auto eco = economy.GetState();
    maybe_resize_world_from_economy(eco);

    ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"amount\":" << *amount << ","
      << "\"balance_mi\":" << balance_after << ","
      << "\"period_key\":\"" << json_escape(period_key) << "\","
      << "\"economy\":{"
      << "\"M\":" << eco.state.m << ","
      << "\"K\":" << eco.state.k << ","
      << "\"A_world\":" << eco.state.a_world << ","
      << "\"M_white\":" << eco.state.m_white << ","
      << "\"P\":" << json_number(eco.state.p) << ","
      << "\"pi\":" << json_number(eco.state.pi)
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
      res.set_content("{\"error\":\"snake_not_found\"}", "application/json");
      return;
    }
    if (snake->owner_user_id != uid_str) {
      res.status = 403;
      res.set_content("{\"error\":\"forbidden\"}", "application/json");
      return;
    }

    int64_t balance_after = 0;
    int64_t snake_len_after = 0;
    if (!storage->AttachCellsToSnake(uid_str, snake_id_str, *amount, balance_after, snake_len_after)) {
      res.status = 409;
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

    ostringstream o;
    o << "{"
      << "\"ok\":true,"
      << "\"balance_mi\":" << balance_after << ","
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

  srv.Post("/auth/login", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
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

  srv.Get("/me/snakes", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    auto snakes = game.list_user_snakes(*uid);
    ostringstream o;
    o << "{\"snakes\":[";
    for (size_t i = 0; i < snakes.size(); ++i) {
      o << "{" << "\"id\":" << snakes[i].id << ","
        << "\"color\":\"" << json_escape(snakes[i].color) << "\"," 
        << "\"paused\":" << (snakes[i].paused ? "true" : "false") << ","
        << "\"len\":" << snakes[i].body.size() << "}";
      if (i + 1 < snakes.size()) o << ",";
    }
    o << "]}";
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
    string color = c ? *c : "#ff00ff";
    const string uid_str = std::to_string(*uid);

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
    economy.InvalidateCache();

    const auto user_after = storage->GetUserById(uid_str);
    const int64_t balance_after = user_after ? user_after->balance_mi : 0;

    ostringstream o;
    o << "{"
      << "\"id\":" << *id << ","
      << "\"balance_mi\":" << balance_after
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
  game.flush_persistence_delta();
  Aws::ShutdownAPI(aws_options);
  return 0;
}
