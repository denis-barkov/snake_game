// snake_server.cpp
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
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
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <sqlite3.h>
#include "httplib.h"
#include "protocol/encode_json.h"
#include "../config/runtime_config.h"

using namespace std;

static constexpr int DEFAULT_W = 40;
static constexpr int DEFAULT_H = 20;

static int GRID_W = DEFAULT_W;
static int GRID_H = DEFAULT_H;

static int FOOD_COUNT = 1;

static int MAX_SNAKES_PER_USER = 3; // env override

// -------------------- Utilities --------------------

static uint64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

static string json_escape(const string& s) {
  ostringstream o;
  for (char c : s) {
    switch (c) {
      case '\"': o << "\\\""; break;
      case '\\': o << "\\\\"; break;
      case '\b': o << "\\b"; break;
      case '\f': o << "\\f"; break;
      case '\n': o << "\\n"; break;
      case '\r': o << "\\r"; break;
      case '\t': o << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          o << "\\u" << hex << setw(4) << setfill('0') << (int)c;
        } else {
          o << c;
        }
    }
  }
  return o.str();
}

static string rand_token(size_t n = 32) {
  static const char* chars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  static thread_local std::mt19937 rng((uint32_t)std::random_device{}());
  std::uniform_int_distribution<int> dist(0, (int)strlen(chars) - 1);
  string t; t.reserve(n);
  for (size_t i = 0; i < n; i++) t.push_back(chars[dist(rng)]);
  return t;
}

struct Vec2 {
  int x = 0;
  int y = 0;
  bool operator==(const Vec2& o) const { return x == o.x && y == o.y; }
};

struct Vec2Hash {
  size_t operator()(const Vec2& v) const noexcept {
    return (size_t)v.x * 1315423911u + (size_t)v.y;
  }
};

enum class Dir : int { Stop=0, Left=1, Right=2, Up=3, Down=4 };

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
    case Dir::Left:  p.x--; break;
    case Dir::Right: p.x++; break;
    case Dir::Up:    p.y--; break;
    case Dir::Down:  p.y++; break;
    default: break;
  }
  // wrap
  if (p.x < 0) p.x = GRID_W - 1;
  if (p.x >= GRID_W) p.x = 0;
  if (p.y < 0) p.y = GRID_H - 1;
  if (p.y >= GRID_H) p.y = 0;
  return p;
}

// -------------------- DB --------------------

struct DB {
  sqlite3* db = nullptr;

  bool open(const string& path) {
    if (sqlite3_open(path.c_str(), &db) != SQLITE_OK) return false;
    return true;
  }
  void close() {
    if (db) sqlite3_close(db);
    db = nullptr;
  }

  bool exec(const string& sql) {
    char* err = nullptr;
    int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &err);
    if (rc != SQLITE_OK) {
      if (err) {
        cerr << "DB error: " << err << endl;
        sqlite3_free(err);
      }
      return false;
    }
    return true;
  }

  void init_schema() {
    exec(R"sql(
      PRAGMA journal_mode=WAL;
      PRAGMA synchronous=NORMAL;

      CREATE TABLE IF NOT EXISTS users(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        username TEXT UNIQUE NOT NULL,
        password TEXT NOT NULL -- (MVP) plaintext; replace with hash in prod
      );

      CREATE TABLE IF NOT EXISTS tokens(
        token TEXT PRIMARY KEY,
        user_id INTEGER NOT NULL,
        created_at INTEGER NOT NULL,
        FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
      );

      CREATE TABLE IF NOT EXISTS snakes(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        user_id INTEGER NOT NULL,
        color TEXT NOT NULL,
        dir INTEGER NOT NULL,
        paused INTEGER NOT NULL,
        alive INTEGER NOT NULL,
        grow INTEGER NOT NULL,
        -- body as "x,y;x,y;..."
        body TEXT NOT NULL,
        updated_at INTEGER NOT NULL,
        FOREIGN KEY(user_id) REFERENCES users(id) ON DELETE CASCADE
      );

      CREATE TABLE IF NOT EXISTS food(
        id INTEGER PRIMARY KEY AUTOINCREMENT,
        x INTEGER NOT NULL,
        y INTEGER NOT NULL
      );
    )sql");
  }

  void reset_all() {
    exec("DROP TABLE IF EXISTS tokens;");
    exec("DROP TABLE IF EXISTS snakes;");
    exec("DROP TABLE IF EXISTS food;");
    exec("DROP TABLE IF EXISTS users;");
    init_schema();
  }
};

// -------------------- Game Model --------------------

struct Snake {
  int id = 0;
  int user_id = 0;
  string color = "#00ff00";
  Dir dir = Dir::Stop;
  bool paused = false;
  bool alive = true;
  int grow = 0; // pending growth cells
  vector<Vec2> body; // body[0] = head
};

struct User {
  int id = 0;
  string username;
};

struct GameState {
  vector<Snake> snakes;
  vector<Vec2> foods;
  uint64_t tick = 0;
};

class GameEngine {
public:
  explicit GameEngine(DB& db) : db_(db) {}

  void load_from_db_or_seed_positions() {
    lock_guard<mutex> lock(mu_);

    snakes_.clear();
    foods_.clear();

    // Load snakes
    {
      const char* sql = "SELECT id,user_id,color,dir,paused,alive,grow,body FROM snakes;";
      sqlite3_stmt* stmt = nullptr;
      sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        Snake s;
        s.id = sqlite3_column_int(stmt, 0);
        s.user_id = sqlite3_column_int(stmt, 1);
        s.color = (const char*)sqlite3_column_text(stmt, 2);
        s.dir = (Dir)sqlite3_column_int(stmt, 3);
        s.paused = sqlite3_column_int(stmt, 4) != 0;
        s.alive = sqlite3_column_int(stmt, 5) != 0;
        s.grow = sqlite3_column_int(stmt, 6);
        string body = (const char*)sqlite3_column_text(stmt, 7);
        s.body = parse_body(body);
        if (s.body.empty()) s.body.push_back(rand_free_cell_locked());
        snakes_.push_back(std::move(s));
      }
      sqlite3_finalize(stmt);
    }

    // Load food
    {
      const char* sql = "SELECT x,y FROM food;";
      sqlite3_stmt* stmt = nullptr;
      sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
      while (sqlite3_step(stmt) == SQLITE_ROW) {
        foods_.push_back({sqlite3_column_int(stmt, 0), sqlite3_column_int(stmt, 1)});
      }
      sqlite3_finalize(stmt);
    }

    // Ensure at least FOOD_COUNT food items
    while ((int)foods_.size() < FOOD_COUNT) foods_.push_back(rand_free_cell_locked());
    persist_food_locked();

    // Ensure snakes don't overlap on startup (if DB dirty)
    resolve_overlaps_on_start_locked();
    persist_all_snakes_locked();
  }

  // Called every tick
  void tick() {
    lock_guard<mutex> lock(mu_);
    tick_++;

    // Build occupancy map for collision checks (excluding heads moving)
    unordered_map<int, unordered_set<Vec2, Vec2Hash>> snake_cells;
    unordered_set<Vec2, Vec2Hash> occupied;

    for (auto& s : snakes_) {
      if (!s.alive) continue;
      for (auto& c : s.body) {
        snake_cells[s.id].insert(c);
        occupied.insert(c);
      }
    }

    // 1) Move each snake head (if not paused, not stop)
    // We compute next positions first (simultaneous-ish)
    unordered_map<int, Vec2> next_head;
    for (auto& s : snakes_) {
      if (!s.alive) continue;
      if (s.paused) continue;
      if (s.dir == Dir::Stop) continue;
      next_head[s.id] = step(s.body[0], s.dir);
    }

    // 2) Apply movements (push new head, pop tail unless growing)
    for (auto& s : snakes_) {
      if (!s.alive) continue;
      auto it = next_head.find(s.id);
      if (it == next_head.end()) continue;

      Vec2 nh = it->second;
      s.body.insert(s.body.begin(), nh);

      if (s.grow > 0) {
        s.grow--;
      } else {
        // normal move: remove last
        if (!s.body.empty()) s.body.pop_back();
      }
    }

    // 3) Handle self-hit: if head overlaps own body (after move)
    // Requirement: loses one cell and stops until owner gives resolving direction
    for (auto& s : snakes_) {
      if (!s.alive) continue;
      if (s.body.size() < 2) continue;
      Vec2 h = s.body[0];
      bool hit_self = false;
      for (size_t i = 1; i < s.body.size(); i++) {
        if (s.body[i] == h) { hit_self = true; break; }
      }
      if (hit_self) {
        // lose one cell (tail)
        if (!s.body.empty()) s.body.pop_back();
        s.paused = true;
        // keep direction as-is, but won't move until user sends direction (we unpause on direction command)
        if (s.body.empty()) {
          s.alive = false;
        }
      }
    }

    // 4) Handle snake vs snake collisions:
    // If head hits any other snake cell:
    // - attacker grows by 1 and reverses direction (opposite) to avoid immediate repeat
    // - defender shrinks by 1 (tail), continues direction
    // - snake dies only if loses last cell
    // NOTE: If multiple collisions occur in same tick, we process deterministically by snake id.
    // Build a map of cell->snake owners (after movement)
    unordered_map<long long, vector<int>> cellOwners; // key->snake ids
    auto key = [](Vec2 v) -> long long { return ((long long)v.x << 32) ^ (unsigned long long)(v.y & 0xffffffff); };

    for (auto& s : snakes_) {
      if (!s.alive) continue;
      for (auto& c : s.body) cellOwners[key(c)].push_back(s.id);
    }

    // Helper: find snake by id
    auto findSnake = [&](int sid) -> Snake* {
      for (auto& s : snakes_) if (s.id == sid) return &s;
      return nullptr;
    };

    // collisions (head into others)
    vector<int> snakeIds;
    snakeIds.reserve(snakes_.size());
    for (auto& s : snakes_) if (s.alive) snakeIds.push_back(s.id);
    sort(snakeIds.begin(), snakeIds.end());

    for (int sid : snakeIds) {
      Snake* attacker = findSnake(sid);
      if (!attacker || !attacker->alive) continue;
      if (attacker->body.empty()) { attacker->alive = false; continue; }

      Vec2 h = attacker->body[0];

      // Who owns this cell?
      auto it = cellOwners.find(key(h));
      if (it == cellOwners.end()) continue;

      // If this cell contains only attacker cells, ignore here (self handled above)
      // Otherwise, any other snake present -> collision
      bool hit_other = false;
      int defenderId = 0;
      for (int ownerId : it->second) {
        if (ownerId != attacker->id) { hit_other = true; defenderId = ownerId; break; }
      }
      if (!hit_other) continue;

      Snake* defender = findSnake(defenderId);
      if (!defender || !defender->alive) continue;

      // Attacker: grow 1, reverse direction and keep moving (unless paused)
      attacker->grow += 1;
      attacker->dir = opposite(attacker->dir);
      attacker->paused = false; // attacker keeps going

      // Defender: lose one cell (tail)
      if (!defender->body.empty()) defender->body.pop_back();
      if (defender->body.empty()) {
        defender->alive = false;
      }
      // Defender keeps direction and paused state unchanged
    }

    // Remove dead snakes from DB (owner loses forever)
    bool any_removed = false;
    for (auto& s : snakes_) {
      if (!s.alive) {
        delete_snake_locked(s.id);
        any_removed = true;
      }
    }
    if (any_removed) {
      snakes_.erase(remove_if(snakes_.begin(), snakes_.end(), [](const Snake& s){ return !s.alive; }), snakes_.end());
    }

    // 5) Food: if head hits food -> grow + respawn food at free cell
    for (auto& s : snakes_) {
      if (!s.alive) continue;
      if (s.body.empty()) continue;
      Vec2 h = s.body[0];

      for (auto& f : foods_) {
        if (f == h) {
          s.grow += 1;
          f = rand_free_cell_locked();
        }
      }
    }
    persist_food_locked();

    // 6) Persist snakes occasionally (or every tick for correctness MVP)
    persist_all_snakes_locked();
  }

  // Public snapshot for API/broadcast
  GameState snapshot() {
    lock_guard<mutex> lock(mu_);
    GameState gs;
    gs.tick = tick_;
    gs.snakes = snakes_;
    gs.foods = foods_;
    return gs;
  }

  // Auth user controls
  bool set_snake_dir(int user_id, int snake_id, Dir d) {
    lock_guard<mutex> lock(mu_);
    Snake* s = find_snake_locked(snake_id);
    if (!s || s->user_id != user_id) return false;
    s->dir = d;
    s->paused = false; // requirement: self-hit stops until owner gives resolving direction
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
    for (auto& s : snakes_) if (s.user_id == user_id) out.push_back(s);
    return out;
  }

  // Create snake (limited by env)
  optional<int> create_snake_for_user(int user_id, const string& color) {
    lock_guard<mutex> lock(mu_);
    int count = 0;
    for (auto& s : snakes_) if (s.user_id == user_id) count++;
    if (count >= MAX_SNAKES_PER_USER) return nullopt;

    Snake s;
    s.user_id = user_id;
    s.color = color;
    s.dir = Dir::Stop;
    s.paused = false;
    s.alive = true;
    s.grow = 0;
    s.body = { rand_free_cell_locked() };

    int new_id = insert_snake_locked(s);
    s.id = new_id;
    snakes_.push_back(s);
    return new_id;
  }

private:
  DB& db_;
  mutex mu_;
  uint64_t tick_ = 0;
  vector<Snake> snakes_;
  vector<Vec2> foods_;

  vector<Vec2> parse_body(const string& s) {
    vector<Vec2> out;
    if (s.empty()) return out;
    size_t start = 0;
    while (start < s.size()) {
      size_t semi = s.find(';', start);
      string part = (semi == string::npos) ? s.substr(start) : s.substr(start, semi - start);
      size_t comma = part.find(',');
      if (comma != string::npos) {
        int x = stoi(part.substr(0, comma));
        int y = stoi(part.substr(comma + 1));
        out.push_back({x, y});
      }
      if (semi == string::npos) break;
      start = semi + 1;
    }
    return out;
  }

  string serialize_body(const vector<Vec2>& body) {
    ostringstream oss;
    for (size_t i = 0; i < body.size(); i++) {
      oss << body[i].x << "," << body[i].y;
      if (i + 1 < body.size()) oss << ";";
    }
    return oss.str();
  }

  Snake* find_snake_locked(int snake_id) {
    for (auto& s : snakes_) if (s.id == snake_id) return &s;
    return nullptr;
  }

  Vec2 rand_free_cell_locked() {
    static thread_local std::mt19937 rng((uint32_t)std::random_device{}());
    std::uniform_int_distribution<int> dx(0, GRID_W - 1);
    std::uniform_int_distribution<int> dy(0, GRID_H - 1);

    unordered_set<long long> occ;
    auto key = [](Vec2 v) -> long long { return ((long long)v.x << 32) ^ (unsigned long long)(v.y & 0xffffffff); };
    for (auto& s : snakes_) {
      if (!s.alive) continue;
      for (auto& c : s.body) occ.insert(key(c));
    }
    for (auto& f : foods_) occ.insert(key(f));

    for (int tries = 0; tries < 2000; tries++) {
      Vec2 v{dx(rng), dy(rng)};
      if (!occ.count(key(v))) return v;
    }
    return {0, 0}; // fallback
  }

  void resolve_overlaps_on_start_locked() {
    unordered_set<long long> occ;
    auto key = [](Vec2 v) -> long long { return ((long long)v.x << 32) ^ (unsigned long long)(v.y & 0xffffffff); };
    for (auto& s : snakes_) {
      if (!s.alive) continue;
      if (s.body.empty()) s.body.push_back(rand_free_cell_locked());

      // If any body cell collides with already occupied, respawn snake as 1-cell
      bool bad = false;
      for (auto& c : s.body) if (occ.count(key(c))) { bad = true; break; }
      if (bad) {
        s.body = { rand_free_cell_locked() };
        s.grow = 0;
        s.dir = Dir::Stop;
        s.paused = false;
      }
      for (auto& c : s.body) occ.insert(key(c));
    }
  }

  int insert_snake_locked(const Snake& s) {
    const char* sql = "INSERT INTO snakes(user_id,color,dir,paused,alive,grow,body,updated_at) VALUES(?,?,?,?,?,?,?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, s.user_id);
    sqlite3_bind_text(stmt, 2, s.color.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 3, (int)s.dir);
    sqlite3_bind_int(stmt, 4, s.paused ? 1 : 0);
    sqlite3_bind_int(stmt, 5, s.alive ? 1 : 0);
    sqlite3_bind_int(stmt, 6, s.grow);
    string body = serialize_body(s.body);
    sqlite3_bind_text(stmt, 7, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 8, (sqlite3_int64)time(nullptr));
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (int)sqlite3_last_insert_rowid(db_.db);
  }

  void persist_snake_locked(const Snake& s) {
    const char* sql = "UPDATE snakes SET color=?,dir=?,paused=?,alive=?,grow=?,body=?,updated_at=? WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
    sqlite3_bind_text(stmt, 1, s.color.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, (int)s.dir);
    sqlite3_bind_int(stmt, 3, s.paused ? 1 : 0);
    sqlite3_bind_int(stmt, 4, s.alive ? 1 : 0);
    sqlite3_bind_int(stmt, 5, s.grow);
    string body = serialize_body(s.body);
    sqlite3_bind_text(stmt, 6, body.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 7, (sqlite3_int64)time(nullptr));
    sqlite3_bind_int(stmt, 8, s.id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void persist_all_snakes_locked() {
    for (auto& s : snakes_) persist_snake_locked(s);
  }

  void delete_snake_locked(int snake_id) {
    const char* sql = "DELETE FROM snakes WHERE id=?;";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
    sqlite3_bind_int(stmt, 1, snake_id);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  void persist_food_locked() {
    db_.exec("DELETE FROM food;");
    const char* sql = "INSERT INTO food(x,y) VALUES(?,?);";
    sqlite3_stmt* stmt = nullptr;
    sqlite3_prepare_v2(db_.db, sql, -1, &stmt, nullptr);
    for (auto& f : foods_) {
      sqlite3_reset(stmt);
      sqlite3_bind_int(stmt, 1, f.x);
      sqlite3_bind_int(stmt, 2, f.y);
      sqlite3_step(stmt);
    }
    sqlite3_finalize(stmt);
  }
};

// -------------------- Auth / API --------------------

static optional<int> token_to_user(DB& db, const string& token) {
  const char* sql = "SELECT user_id FROM tokens WHERE token=?;";
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.db, sql, -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc == SQLITE_ROW) {
    int uid = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return uid;
  }
  sqlite3_finalize(stmt);
  return nullopt;
}

static optional<int> user_login(DB& db, const string& username, const string& password, string& out_token) {
  const char* sql = "SELECT id,password FROM users WHERE username=?;";
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.db, sql, -1, &stmt, nullptr);
  sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
  int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) { sqlite3_finalize(stmt); return nullopt; }

  int uid = sqlite3_column_int(stmt, 0);
  string pw = (const char*)sqlite3_column_text(stmt, 1);
  sqlite3_finalize(stmt);

  if (pw != password) return nullopt;

  out_token = rand_token();
  const char* ins = "INSERT INTO tokens(token,user_id,created_at) VALUES(?,?,?);";
  sqlite3_stmt* ins_stmt = nullptr;
  sqlite3_prepare_v2(db.db, ins, -1, &ins_stmt, nullptr);
  sqlite3_bind_text(ins_stmt, 1, out_token.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(ins_stmt, 2, uid);
  sqlite3_bind_int64(ins_stmt, 3, (sqlite3_int64)time(nullptr));
  sqlite3_step(ins_stmt);
  sqlite3_finalize(ins_stmt);

  return uid;
}

static int count_user_snakes(DB& db, int uid) {
  const char* sql = "SELECT COUNT(*) FROM snakes WHERE user_id=?;";
  sqlite3_stmt* stmt = nullptr;
  sqlite3_prepare_v2(db.db, sql, -1, &stmt, nullptr);
  sqlite3_bind_int(stmt, 1, uid);
  int rc = sqlite3_step(stmt);
  int c = 0;
  if (rc == SQLITE_ROW) c = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  return c;
}

// -------------------- JSON helpers (minimal) --------------------
// NOTE: This is intentionally minimal. For production, use a JSON lib.

static optional<string> get_json_string_field(const string& body, const string& key) {
  // naive: finds "key":"value"
  string pat = "\"" + key + "\"";
  size_t p = body.find(pat);
  if (p == string::npos) return nullopt;
  p = body.find(':', p);
  if (p == string::npos) return nullopt;
  p++;
  while (p < body.size() && isspace((unsigned char)body[p])) p++;
  if (p >= body.size() || body[p] != '"') return nullopt;
  p++;
  size_t e = body.find('"', p);
  if (e == string::npos) return nullopt;
  return body.substr(p, e - p);
}

static optional<int> get_json_int_field(const string& body, const string& key) {
  string pat = "\"" + key + "\"";
  size_t p = body.find(pat);
  if (p == string::npos) return nullopt;
  p = body.find(':', p);
  if (p == string::npos) return nullopt;
  p++;
  while (p < body.size() && isspace((unsigned char)body[p])) p++;
  size_t e = p;
  while (e < body.size() && (isdigit((unsigned char)body[e]) || body[e] == '-')) e++;
  if (e == p) return nullopt;
  return stoi(body.substr(p, e - p));
}

// -------------------- Server --------------------

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

static optional<int> require_auth_user(DB& db, const httplib::Request& req) {
  auto it = req.headers.find("Authorization");
  if (it == req.headers.end()) return nullopt;
  const string& v = it->second;
  // expects: Bearer TOKEN
  const string prefix = "Bearer ";
  if (v.rfind(prefix, 0) != 0) return nullopt;
  return token_to_user(db, v.substr(prefix.size()));
}

static void add_cors(httplib::Response& res) {
  res.set_header("Access-Control-Allow-Origin", "*");
  res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
  res.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization");
}

static void seed(DB& db, GameEngine& game) {
  // two users with simple passwords
  db.exec("INSERT OR IGNORE INTO users(username,password) VALUES('user1','pass1');");
  db.exec("INSERT OR IGNORE INTO users(username,password) VALUES('user2','pass2');");

  auto get_uid = [&](const string& u)->int{
    const char* sql="SELECT id FROM users WHERE username=?;";
    sqlite3_stmt* st=nullptr;
    sqlite3_prepare_v2(db.db, sql, -1, &st, nullptr);
    sqlite3_bind_text(st,1,u.c_str(),-1,SQLITE_TRANSIENT);
    int rc=sqlite3_step(st);
    int id=0;
    if(rc==SQLITE_ROW) id=sqlite3_column_int(st,0);
    sqlite3_finalize(st);
    return id;
  };

  int u1 = get_uid("user1");
  int u2 = get_uid("user2");

  // create one 1-cell snake each if none exists
  if (count_user_snakes(db, u1) == 0) {
    game.create_snake_for_user(u1, "#00ff00");
  }
  if (count_user_snakes(db, u2) == 0) {
    game.create_snake_for_user(u2, "#00aaff");
  }

  game.load_from_db_or_seed_positions();
  cout << "Seeded users: user1/pass1, user2/pass2 (1 snake each)\n";
}

int main(int argc, char** argv) {
  string mode = (argc >= 2) ? argv[1] : "serve";

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

  DB db;
  if (!db.open("snake.db")) {
    cerr << "Failed to open DB\n";
    return 1;
  }
  db.init_schema();

  GameEngine game(db);
  game.load_from_db_or_seed_positions();

  if (mode == "reset") {
    db.reset_all();
    cout << "DB reset.\n";
    return 0;
  }
  if (mode == "seed") {
    seed(db, game);
    return 0;
  }
  if (mode != "serve") {
    cerr << "Usage: ./snake_server [serve|seed|reset]\n";
    return 1;
  }

  // Game loop thread: simulation runs at tick_hz, snapshots publish at spectator_hz.
  atomic<bool> running{true};
  mutex snapshot_mu;
  string latest_snapshot = state_to_json(game.snapshot());
  uint64_t snapshot_seq = 1;

  thread loop([&]{
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

  httplib::Server srv;

  srv.Options(R"(.*)", [&](const httplib::Request&, httplib::Response& res){
    add_cors(res);
    res.status = 204;
  });

  // Public: state snapshot (polling fallback)
  srv.Get("/game/state", [&](const httplib::Request&, httplib::Response& res){
    add_cors(res);
    auto gs = game.snapshot();
    res.set_content(state_to_json(gs), "application/json");
  });

  // Public: SSE stream for watchers (very low overhead, one-way)
  srv.Get("/game/stream", [&](const httplib::Request&, httplib::Response& res){
    add_cors(res);
    res.set_header("Cache-Control", "no-cache");
    res.set_header("Connection", "keep-alive");
    res.set_header("Content-Type", "text/event-stream");

    // stream provider
    res.set_chunked_content_provider(
      "text/event-stream",
      [&](size_t /*offset*/, httplib::DataSink& sink) {
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
          if (!payload.empty()) {
            if (!sink.write(payload.data(), payload.size())) break;
          }
          int poll_ms = max(1, runtime_cfg.SpectatorIntervalMs() / 2);
          this_thread::sleep_for(chrono::milliseconds(poll_ms));
        }
        sink.done();
        return true;
      }
    );
  });

  // Auth: login (returns token)
  // body: {"username":"user1","password":"pass1"}
  srv.Post("/auth/login", [&](const httplib::Request& req, httplib::Response& res){
    add_cors(res);
    auto u = get_json_string_field(req.body, "username");
    auto p = get_json_string_field(req.body, "password");
    if (!u || !p) { res.status = 400; res.set_content("{\"error\":\"bad_request\"}", "application/json"); return; }

    string token;
    auto uid = user_login(db, *u, *p, token);
    if (!uid) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

    ostringstream o;
    o << "{\"token\":\"" << json_escape(token) << "\",\"user_id\":" << *uid << "}";
    res.set_content(o.str(), "application/json");
  });

  // Auth: list my snakes
  srv.Get("/me/snakes", [&](const httplib::Request& req, httplib::Response& res){
    add_cors(res);
    auto uid = require_auth_user(db, req);
    if (!uid) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

    auto snakes = game.list_user_snakes(*uid);
    ostringstream o;
    o << "{\"snakes\":[";
    for (size_t i=0;i<snakes.size();i++){
      o << "{"
        << "\"id\":"<<snakes[i].id<<","
        << "\"color\":\""<<json_escape(snakes[i].color)<<"\","
        << "\"paused\":"<<(snakes[i].paused?"true":"false")<<","
        << "\"len\":"<<snakes[i].body.size()
        << "}";
      if (i+1<snakes.size()) o << ",";
    }
    o << "]}";
    res.set_content(o.str(), "application/json");
  });

  // Auth: set snake direction (W/A/S/D) -> 1..4
  // POST /snakes/:id/dir body: {"dir":2}
  srv.Post(R"(/snakes/(\d+)/dir)", [&](const httplib::Request& req, httplib::Response& res){
    add_cors(res);
    auto uid = require_auth_user(db, req);
    if (!uid) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

    int snake_id = stoi(req.matches[1]);
    auto d = get_json_int_field(req.body, "dir");
    if (!d || *d < 1 || *d > 4) { res.status = 400; res.set_content("{\"error\":\"bad_dir\"}", "application/json"); return; }

    bool ok = game.set_snake_dir(*uid, snake_id, (Dir)*d);
    if (!ok) { res.status = 403; res.set_content("{\"error\":\"forbidden\"}", "application/json"); return; }
    res.set_content("{\"status\":\"OK\"}", "application/json");
  });

  // Auth: toggle pause for my snake (P)
  srv.Post(R"(/snakes/(\d+)/pause)", [&](const httplib::Request& req, httplib::Response& res){
    add_cors(res);
    auto uid = require_auth_user(db, req);
    if (!uid) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }
    int snake_id = stoi(req.matches[1]);
    bool ok = game.toggle_snake_pause(*uid, snake_id);
    if (!ok) { res.status = 403; res.set_content("{\"error\":\"forbidden\"}", "application/json"); return; }
    res.set_content("{\"status\":\"OK\"}", "application/json");
  });

  // Auth: create snake (MVP)
  // POST /me/snakes body: {"color":"#ff00ff"}
  srv.Post("/me/snakes", [&](const httplib::Request& req, httplib::Response& res){
    add_cors(res);
    auto uid = require_auth_user(db, req);
    if (!uid) { res.status = 401; res.set_content("{\"error\":\"unauthorized\"}", "application/json"); return; }

    auto c = get_json_string_field(req.body, "color");
    string color = c ? *c : "#ff00ff";

    auto id = game.create_snake_for_user(*uid, color);
    if (!id) { res.status = 429; res.set_content("{\"error\":\"snake_limit\"}", "application/json"); return; }
    ostringstream o; o << "{\"id\":"<<*id<<"}";
    res.set_content(o.str(), "application/json");
  });

  cout << "Server on http://127.0.0.1:8080\n";
  cout << "SSE:   GET /game/stream\n";
  cout << "State: GET /game/state\n";
  cout << "Login: POST /auth/login {username,password}\n";

  srv.listen("127.0.0.1", 8080);

  running.store(false);
  loop.join();
  db.close();
  return 0;
}
