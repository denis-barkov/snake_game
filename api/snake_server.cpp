// snake_server.cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <ctime>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <aws/core/Aws.h>

#include "httplib.h"
#include "protocol/encode_json.h"
#include "storage/storage_factory.h"
#include "../config/runtime_config.h"

using namespace std;

static constexpr int DEFAULT_W = 40;
static constexpr int DEFAULT_H = 20;

static int GRID_W = DEFAULT_W;
static int GRID_H = DEFAULT_H;

static int FOOD_COUNT = 1;
static int MAX_SNAKES_PER_USER = 3;

static uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static string json_escape(const string& s) {
  ostringstream o;
  for (char c : s) {
    switch (c) {
      case '"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\b': o << "\\b"; break;
      case '\f': o << "\\f"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
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

static string rand_token(size_t n = 32) {
  static const char* chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static thread_local mt19937 rng(static_cast<uint32_t>(random_device{}()));
  uniform_int_distribution<int> dist(0, static_cast<int>(strlen(chars)) - 1);
  string t;
  t.reserve(n);
  for (size_t i = 0; i < n; ++i) t.push_back(chars[dist(rng)]);
  return t;
}

struct Vec2 {
  int x = 0;
  int y = 0;
  bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};

struct Vec2Hash {
  size_t operator()(const Vec2& v) const noexcept {
    return static_cast<size_t>(v.x) * 1315423911u + static_cast<size_t>(v.y);
  }
};

enum class Dir : int { Stop = 0, Left = 1, Right = 2, Up = 3, Down = 4 };

static Dir opposite(Dir d) {
  switch (d) {
    case Dir::Left: return Dir::Right;
    case Dir::Right: return Dir::Left;
    case Dir::Up: return Dir::Down;
    case Dir::Down: return Dir::Up;
    default: return Dir::Stop;
  }
}

static Vec2 step(Vec2 p, Dir d) {
  switch (d) {
    case Dir::Left: --p.x; break;
    case Dir::Right: ++p.x; break;
    case Dir::Up: --p.y; break;
    case Dir::Down: ++p.y; break;
    default: break;
  }
  if (p.x < 0) p.x = GRID_W - 1;
  if (p.x >= GRID_W) p.x = 0;
  if (p.y < 0) p.y = GRID_H - 1;
  if (p.y >= GRID_H) p.y = 0;
  return p;
}

struct Snake {
  int id = 0;
  int user_id = 0;
  string color = "#00ff00";
  Dir dir = Dir::Stop;
  bool paused = false;
  bool alive = true;
  int grow = 0;
  vector<Vec2> body;
};

struct GameState {
  vector<Snake> snakes;
  vector<Vec2> foods;
  uint64_t tick = 0;
};

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

class GameEngine {
 public:
  explicit GameEngine(storage::IStorage& storage) : storage_(storage) {}

  void load_from_storage_or_seed_positions() {
    lock_guard<mutex> lock(mu_);

    snakes_.clear();
    foods_.clear();

    auto checkpoints = storage_.ListLatestSnakeCheckpoints();
    int max_snake_id = 0;

    for (const auto& cp : checkpoints) {
      if (cp.body.empty() || cp.length <= 0) continue;
      Snake s;
      s.id = to_int(cp.snake_id);
      s.user_id = to_int(cp.owner_user_id);
      s.dir = static_cast<Dir>(cp.dir);
      s.paused = cp.paused;
      s.alive = true;
      s.grow = 0;
      s.color = color_for_user(s.user_id);
      s.body.reserve(cp.body.size());
      for (const auto& cell : cp.body) {
        s.body.push_back({cell.first, cell.second});
      }
      if (!s.body.empty() && s.id > 0 && s.user_id > 0) {
        snakes_.push_back(std::move(s));
        max_snake_id = max(max_snake_id, snakes_.back().id);
      }
    }

    next_snake_id_ = max_snake_id + 1;
    while (static_cast<int>(foods_.size()) < FOOD_COUNT) foods_.push_back(rand_free_cell_locked());
    resolve_overlaps_on_start_locked();
    persist_all_snakes_locked();
  }

  void tick() {
    lock_guard<mutex> lock(mu_);
    ++tick_;

    unordered_map<long long, vector<int>> cellOwners;
    auto key = [](Vec2 v) -> long long {
      return (static_cast<long long>(v.x) << 32) ^ static_cast<unsigned long long>(v.y & 0xffffffff);
    };

    unordered_map<int, Vec2> next_head;
    for (auto& s : snakes_) {
      if (!s.alive || s.paused || s.dir == Dir::Stop || s.body.empty()) continue;
      next_head[s.id] = step(s.body[0], s.dir);
    }

    for (auto& s : snakes_) {
      if (!s.alive) continue;
      auto it = next_head.find(s.id);
      if (it == next_head.end()) continue;

      s.body.insert(s.body.begin(), it->second);
      if (s.grow > 0) {
        --s.grow;
      } else if (!s.body.empty()) {
        s.body.pop_back();
      }
    }

    for (auto& s : snakes_) {
      if (!s.alive || s.body.size() < 2) continue;
      const Vec2 h = s.body[0];
      bool hit_self = false;
      for (size_t i = 1; i < s.body.size(); ++i) {
        if (s.body[i] == h) {
          hit_self = true;
          break;
        }
      }
      if (hit_self) {
        if (!s.body.empty()) s.body.pop_back();
        s.paused = true;
        if (s.body.empty()) s.alive = false;
      }
    }

    for (auto& s : snakes_) {
      if (!s.alive) continue;
      for (auto& c : s.body) cellOwners[key(c)].push_back(s.id);
    }

    vector<int> snake_ids;
    snake_ids.reserve(snakes_.size());
    for (const auto& s : snakes_) {
      if (s.alive) snake_ids.push_back(s.id);
    }
    sort(snake_ids.begin(), snake_ids.end());

    for (int sid : snake_ids) {
      Snake* attacker = find_snake_locked(sid);
      if (!attacker || !attacker->alive || attacker->body.empty()) continue;
      const Vec2 h = attacker->body[0];

      auto it = cellOwners.find(key(h));
      if (it == cellOwners.end()) continue;

      int defender_id = 0;
      for (int ownerId : it->second) {
        if (ownerId != attacker->id) {
          defender_id = ownerId;
          break;
        }
      }
      if (defender_id == 0) continue;

      Snake* defender = find_snake_locked(defender_id);
      if (!defender || !defender->alive) continue;

      attacker->grow += 1;
      attacker->dir = opposite(attacker->dir);
      attacker->paused = false;

      if (!defender->body.empty()) defender->body.pop_back();
      if (defender->body.empty()) defender->alive = false;
    }

    for (const auto& s : snakes_) {
      if (!s.alive) persist_dead_snake_locked(s);
    }
    snakes_.erase(remove_if(snakes_.begin(), snakes_.end(), [](const Snake& s) { return !s.alive; }), snakes_.end());

    for (auto& s : snakes_) {
      if (!s.alive || s.body.empty()) continue;
      const Vec2 h = s.body[0];
      for (auto& f : foods_) {
        if (f == h) {
          s.grow += 1;
          f = rand_free_cell_locked();
        }
      }
    }

    persist_all_snakes_locked();
  }

  GameState snapshot() {
    lock_guard<mutex> lock(mu_);
    GameState gs;
    gs.tick = tick_;
    gs.snakes = snakes_;
    gs.foods = foods_;
    return gs;
  }

  bool set_snake_dir(int user_id, int snake_id, Dir d) {
    lock_guard<mutex> lock(mu_);
    Snake* s = find_snake_locked(snake_id);
    if (!s || s->user_id != user_id) return false;
    s->dir = d;
    s->paused = false;
    persist_snake_locked(*s);
    return true;
  }

  bool toggle_snake_pause(int user_id, int snake_id) {
    lock_guard<mutex> lock(mu_);
    Snake* s = find_snake_locked(snake_id);
    if (!s || s->user_id != user_id) return false;
    s->paused = !s->paused;
    persist_snake_locked(*s);
    return true;
  }

  vector<Snake> list_user_snakes(int user_id) {
    lock_guard<mutex> lock(mu_);
    vector<Snake> out;
    for (const auto& s : snakes_) {
      if (s.user_id == user_id) out.push_back(s);
    }
    return out;
  }

  optional<int> create_snake_for_user(int user_id, const string& color) {
    lock_guard<mutex> lock(mu_);
    int count = 0;
    for (const auto& s : snakes_) {
      if (s.user_id == user_id) ++count;
    }
    if (count >= MAX_SNAKES_PER_USER) return nullopt;

    Snake s;
    s.id = next_snake_id_++;
    s.user_id = user_id;
    s.color = color;
    s.dir = Dir::Stop;
    s.paused = false;
    s.alive = true;
    s.grow = 0;
    s.body = {rand_free_cell_locked()};

    snakes_.push_back(s);
    persist_snake_locked(snakes_.back());
    return s.id;
  }

 private:
  static int to_int(const string& s) {
    try {
      return stoi(s);
    } catch (...) {
      return 0;
    }
  }

  string color_for_user(int user_id) {
    static const vector<string> palette = {
      "#00ff00", "#00aaff", "#ff00ff", "#ff8800", "#00ffaa", "#ffaa00"
    };
    if (user_id <= 0) return "#00ff00";
    return palette[static_cast<size_t>(user_id - 1) % palette.size()];
  }

  Snake* find_snake_locked(int snake_id) {
    for (auto& s : snakes_) {
      if (s.id == snake_id) return &s;
    }
    return nullptr;
  }

  Vec2 rand_free_cell_locked() {
    static thread_local mt19937 rng(static_cast<uint32_t>(random_device{}()));
    uniform_int_distribution<int> dx(0, GRID_W - 1);
    uniform_int_distribution<int> dy(0, GRID_H - 1);

    unordered_set<long long> occ;
    auto key = [](Vec2 v) -> long long {
      return (static_cast<long long>(v.x) << 32) ^ static_cast<unsigned long long>(v.y & 0xffffffff);
    };

    for (const auto& s : snakes_) {
      if (!s.alive) continue;
      for (const auto& c : s.body) occ.insert(key(c));
    }
    for (const auto& f : foods_) occ.insert(key(f));

    for (int tries = 0; tries < 2000; ++tries) {
      Vec2 v{dx(rng), dy(rng)};
      if (!occ.count(key(v))) return v;
    }
    return {0, 0};
  }

  void resolve_overlaps_on_start_locked() {
    unordered_set<long long> occ;
    auto key = [](Vec2 v) -> long long {
      return (static_cast<long long>(v.x) << 32) ^ static_cast<unsigned long long>(v.y & 0xffffffff);
    };

    for (auto& s : snakes_) {
      if (!s.alive) continue;
      if (s.body.empty()) s.body.push_back(rand_free_cell_locked());

      bool bad = false;
      for (const auto& c : s.body) {
        if (occ.count(key(c))) {
          bad = true;
          break;
        }
      }
      if (bad) {
        s.body = {rand_free_cell_locked()};
        s.grow = 0;
        s.dir = Dir::Stop;
        s.paused = false;
      }
      for (const auto& c : s.body) occ.insert(key(c));
    }
  }

  void persist_snake_locked(const Snake& s) {
    storage::SnakeCheckpoint cp;
    cp.snake_id = to_string(s.id);
    cp.owner_user_id = to_string(s.user_id);
    cp.ts = static_cast<int64_t>(now_ms());
    cp.dir = static_cast<int>(s.dir);
    cp.paused = s.paused;
    cp.length = static_cast<int>(s.body.size());
    cp.score = static_cast<int>(s.body.size());
    cp.w = GRID_W;
    cp.h = GRID_H;
    cp.body.reserve(s.body.size());
    for (const auto& cell : s.body) cp.body.push_back({cell.x, cell.y});
    storage_.PutSnakeCheckpoint(cp);
  }

  void persist_all_snakes_locked() {
    for (const auto& s : snakes_) persist_snake_locked(s);
  }

  void persist_dead_snake_locked(const Snake& s) {
    storage::SnakeCheckpoint cp;
    cp.snake_id = to_string(s.id);
    cp.owner_user_id = to_string(s.user_id);
    cp.ts = static_cast<int64_t>(now_ms());
    cp.dir = static_cast<int>(Dir::Stop);
    cp.paused = true;
    cp.length = 0;
    cp.score = 0;
    cp.w = GRID_W;
    cp.h = GRID_H;
    storage_.PutSnakeCheckpoint(cp);
  }

  storage::IStorage& storage_;
  mutex mu_;
  uint64_t tick_ = 0;
  int next_snake_id_ = 1;
  vector<Snake> snakes_;
  vector<Vec2> foods_;
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

static protocol::Snapshot to_protocol_snapshot(const GameState& gs) {
  protocol::Snapshot snap;
  snap.tick = gs.tick;
  snap.w = GRID_W;
  snap.h = GRID_H;

  snap.foods.reserve(gs.foods.size());
  for (const auto& f : gs.foods) {
    snap.foods.push_back(protocol::Vec2{f.x, f.y});
  }

  snap.snakes.reserve(gs.snakes.size());
  for (const auto& s : gs.snakes) {
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

static string state_to_json(const GameState& gs) {
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

static void seed(storage::IStorage& storage, GameEngine& game) {
  if (!ensure_user(storage, "1", "user1", "pass1") || !ensure_user(storage, "2", "user2", "pass2")) {
    cerr << "Failed to seed users into DynamoDB\n";
    return;
  }

  game.load_from_storage_or_seed_positions();
  if (game.list_user_snakes(1).empty()) game.create_snake_for_user(1, "#00ff00");
  if (game.list_user_snakes(2).empty()) game.create_snake_for_user(2, "#00aaff");
  game.load_from_storage_or_seed_positions();
  cout << "Seeded users: user1/pass1, user2/pass2 (1 snake each)\n";
}

int main(int argc, char** argv) {
  const string mode = (argc >= 2) ? argv[1] : "serve";

  Aws::SDKOptions aws_options;
  Aws::InitAPI(aws_options);

  RuntimeConfig runtime_cfg = RuntimeConfig::FromEnv();

  const char* envW = getenv("SNAKE_W");
  const char* envH = getenv("SNAKE_H");
  const char* envMax = getenv("SNAKE_MAX_PER_USER");

  if (envW) GRID_W = max(10, atoi(envW));
  if (envH) GRID_H = max(10, atoi(envH));
  if (envMax) MAX_SNAKES_PER_USER = max(1, atoi(envMax));

  if (runtime_cfg.log_hz) {
    cout << "RuntimeConfig: "
         << "TICK_HZ=" << runtime_cfg.tick_hz
         << ", SPECTATOR_HZ=" << runtime_cfg.spectator_hz
         << ", PLAYER_HZ=" << runtime_cfg.player_hz
         << ", ENABLE_BROADCAST=" << (runtime_cfg.enable_broadcast ? "true" : "false")
         << "\n";
  }

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

  GameEngine game(*storage);
  game.load_from_storage_or_seed_positions();

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

  thread loop([&] {
    using clock = chrono::steady_clock;
    using ms = chrono::milliseconds;

    const ms tick_dt(runtime_cfg.TickIntervalMs());
    const ms spectator_dt(runtime_cfg.SpectatorIntervalMs());
    auto next_tick = clock::now() + tick_dt;
    auto next_broadcast = clock::now() + spectator_dt;

    uint64_t ticks_since_log = 0;
    uint64_t broadcasts_since_log = 0;
    auto next_log_at = clock::now() + chrono::seconds(5);

    while (running.load()) {
      auto now = clock::now();

      while (now >= next_tick) {
        game.tick();
        ++ticks_since_log;
        next_tick += tick_dt;
        now = clock::now();
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

      if (runtime_cfg.log_hz && now >= next_log_at) {
        cout << "[rate] ticks/5s=" << ticks_since_log
             << ", broadcasts/5s=" << broadcasts_since_log << "\n";
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

  srv.Get("/game/stream", [&](const httplib::Request&, httplib::Response& res) {
    add_cors(res);
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Content-Type", "text/event-stream");

    res.set_chunked_content_provider(
      "text/event-stream",
      [&](size_t, httplib::DataSink& sink) {
        uint64_t last_seq = 0;
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
          if (!payload.empty() && !sink.write(payload.data(), payload.size())) break;
          const int poll_ms = max(1, runtime_cfg.SpectatorIntervalMs() / 2);
          this_thread::sleep_for(chrono::milliseconds(poll_ms));
        }
        sink.done();
        return true;
      }
    );
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

    bool ok = game.set_snake_dir(*uid, snake_id, static_cast<Dir>(*d));
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

    ostringstream o;
    o << "{\"id\":" << *id << "}";
    res.set_content(o.str(), "application/json");
  });

  cout << "Server on http://127.0.0.1:8080\n";
  cout << "SSE:   GET /game/stream\n";
  cout << "State: GET /game/state\n";
  cout << "Login: POST /auth/login {username,password}\n";

  srv.listen("127.0.0.1", 8080);

  running.store(false);
  loop.join();
  Aws::ShutdownAPI(aws_options);
  return 0;
}
