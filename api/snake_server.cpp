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

class GameService {
 public:
  GameService(storage::IStorage& storage, int width, int height, int food_count, int max_snakes_per_user)
      : storage_(storage),
        world_(width, height, food_count, max_snakes_per_user) {}

  void load_from_storage_or_seed_positions() {
    world_.LoadFromStorage(storage_.ListSnakes(), storage_.GetWorldChunk("main"));
  }

  void tick() {
    world_.Tick();
  }

  world::WorldSnapshot snapshot() {
    return world_.Snapshot();
  }

  bool set_snake_dir(int user_id, int snake_id, world::Dir d) {
    return world_.QueueDirectionInput(user_id, snake_id, d);
  }

  bool toggle_snake_pause(int user_id, int snake_id) {
    return world_.QueuePauseToggle(user_id, snake_id);
  }

  vector<world::Snake> list_user_snakes(int user_id) {
    return world_.ListUserSnakes(user_id);
  }

  optional<int> create_snake_for_user(int user_id, const string& color) {
    return world_.CreateSnakeForUser(user_id, color);
  }

  // Writes only event-driven deltas. No per-tick checkpoint persistence.
  void flush_persistence_delta() {
    const auto delta = world_.DrainPersistenceDelta(static_cast<int64_t>(now_ms()));
    if (delta.empty()) return;
    for (const auto& s : delta.upsert_snakes) storage_.PutSnake(s);
    for (const auto& sid : delta.delete_snake_ids) storage_.DeleteSnake(sid);
    if (delta.upsert_world_chunk.has_value()) storage_.PutWorldChunk(*delta.upsert_world_chunk);
    for (const auto& e : delta.snake_events) storage_.AppendSnakeEvent(e);
  }

 private:
  storage::IStorage& storage_;
  world::World world_;
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
  EconomyService economy(*storage);
  game.load_from_storage_or_seed_positions();
  game.flush_persistence_delta();

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
  string latest_snapshot = state_to_json(game.snapshot());
  uint64_t snapshot_seq = 1;

  signal(SIGUSR1, on_reload_signal);
  signal(SIGHUP, on_reload_signal);

  thread loop([&] {
    using clock = chrono::steady_clock;
    using ms = chrono::milliseconds;

    const ms tick_dt(runtime_cfg.TickIntervalMs());
    const ms spectator_dt(runtime_cfg.SpectatorIntervalMs());
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
        string snap = state_to_json(game.snapshot());
        {
          lock_guard<mutex> lock(snapshot_mu);
          latest_snapshot = std::move(snap);
          ++snapshot_seq;
        }
      }

      auto now = clock::now();

      int catch_up_ticks = 0;
      while (now >= next_tick && catch_up_ticks < max_catch_up_ticks) {
        game.tick();
        game.flush_persistence_delta();
        ++ticks_since_log;
        ++catch_up_ticks;
        next_tick += tick_dt;
        now = clock::now();
      }

      if ((now - next_tick) > max_lag) {
        next_tick = now + tick_dt;
      }

      while (runtime_cfg.enable_broadcast && now >= next_broadcast) {
        string snap = state_to_json(game.snapshot());
        {
          lock_guard<mutex> lock(snapshot_mu);
          latest_snapshot = std::move(snap);
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
      << "\"enable_broadcast\":" << (runtime_cfg.enable_broadcast ? "true" : "false")
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/economy/state", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    const auto s = economy.GetState();
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

  srv.Post("/economy/purchase", [&](const httplib::Request& req, httplib::Response& res) {
    add_cors(res);
    auto uid = require_auth_user(auth, req);
    if (!uid) {
      res.status = 401;
      res.set_content("{\"error\":\"unauthorized\"}", "application/json");
      return;
    }

    auto cells = get_json_int_field(req.body, "cells");
    if (!cells) cells = get_json_int_field(req.body, "purchased_cells");
    if (!cells || *cells <= 0) {
      res.status = 400;
      res.set_content("{\"error\":\"bad_cells\"}", "application/json");
      return;
    }

    const string user_id = std::to_string(*uid);
    const string period_key = utc_period_key_yyyymmddhh();

    if (!storage->IncrementUserBalance(user_id, *cells)) {
      res.status = 500;
      res.set_content("{\"error\":\"purchase_user_update_failed\"}", "application/json");
      return;
    }
    if (!storage->IncrementEconomyPeriodDeltaMBuy(period_key, *cells)) {
      // Best-effort compensation for partial failure in non-transactional MVP flow.
      storage->IncrementUserBalance(user_id, -*cells);
      res.status = 500;
      res.set_content("{\"error\":\"purchase_period_update_failed\"}", "application/json");
      return;
    }

    economy.InvalidateCache();
    const auto eco = economy.GetState();
    ostringstream o;
    o << "{"
      << "\"status\":\"OK\","
      << "\"cells\":" << *cells << ","
      << "\"period_key\":\"" << json_escape(period_key) << "\","
      << "\"M\":" << eco.state.m << ","
      << "\"P\":" << json_number(eco.state.p)
      << "}";
    res.set_content(o.str(), "application/json");
  });

  srv.Get("/game/stream", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Content-Type", "text/event-stream");

    res.set_chunked_content_provider(
        "text/event-stream",
        [&](size_t, httplib::DataSink& sink) {
          uint64_t last_seq = 0;
          auto last_heartbeat = chrono::steady_clock::now();
          const auto heartbeat_every = chrono::seconds(10);
          while (true) {
            string payload;
            {
              lock_guard<mutex> lock(snapshot_mu);
              if (snapshot_seq != last_seq) {
                last_seq = snapshot_seq;
                payload = "event: frame\n";
                payload += "data: " + latest_snapshot + "\n\n";
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

    auto id = game.create_snake_for_user(*uid, color);
    if (!id) {
      res.status = 429;
      res.set_content("{\"error\":\"snake_limit\"}", "application/json");
      return;
    }
    game.flush_persistence_delta();

    ostringstream o;
    o << "{\"id\":" << *id << "}";
    res.set_content(o.str(), "application/json");
  });

  cout << "Server on http://" << bind_host << ":" << bind_port << "\n";
  cout << "SSE:   GET /game/stream\n";
  cout << "State: GET /game/state\n";
  cout << "Login: POST /auth/login {username,password}\n";

  srv.listen(bind_host, bind_port);

  running.store(false);
  loop.join();
  game.flush_persistence_delta();
  Aws::ShutdownAPI(aws_options);
  return 0;
}
