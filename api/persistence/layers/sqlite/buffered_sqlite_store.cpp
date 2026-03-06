#include "buffered_sqlite_store.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <utility>
#include <vector>

namespace persistence {
namespace {

int64_t now_ms() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool bind_text(sqlite3_stmt* stmt, int idx, const std::string& s) {
  return sqlite3_bind_text(stmt, idx, s.c_str(), -1, SQLITE_TRANSIENT) == SQLITE_OK;
}

}  // namespace

BufferedSqliteStore::BufferedSqliteStore(std::string sqlite_path)
    : path_(std::move(sqlite_path)) {
  Open();
  EnsureSchema();
}

BufferedSqliteStore::~BufferedSqliteStore() {
  std::lock_guard<std::mutex> lock(mu_);
  if (db_) sqlite3_close(db_);
  db_ = nullptr;
}

bool BufferedSqliteStore::Open() {
  std::lock_guard<std::mutex> lock(mu_);
  if (db_) return true;
  try {
    std::filesystem::path p(path_);
    if (p.has_parent_path()) std::filesystem::create_directories(p.parent_path());
  } catch (...) {
  }
  if (sqlite3_open(path_.c_str(), &db_) != SQLITE_OK) {
    db_ = nullptr;
    return false;
  }
  sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
  sqlite3_exec(db_, "PRAGMA busy_timeout=2000;", nullptr, nullptr, nullptr);
  return true;
}

bool BufferedSqliteStore::Exec(const std::string& sql) {
  char* err = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
  if (err) sqlite3_free(err);
  return rc == SQLITE_OK;
}

bool BufferedSqliteStore::EnsureSchema() {
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) return false;
  return Exec(
             "CREATE TABLE IF NOT EXISTS buffered_snake_events("
             "id INTEGER PRIMARY KEY AUTOINCREMENT,"
             "snake_id TEXT NOT NULL,"
             "event_id TEXT NOT NULL UNIQUE,"
             "event_type TEXT NOT NULL,"
             "x INTEGER NOT NULL,"
             "y INTEGER NOT NULL,"
             "other_snake_id TEXT,"
             "delta_length INTEGER NOT NULL,"
             "tick_number INTEGER NOT NULL,"
             "world_version INTEGER NOT NULL,"
             "created_at INTEGER NOT NULL,"
             "created_ms INTEGER NOT NULL"
             ");") &&
         Exec(
             "CREATE TABLE IF NOT EXISTS buffered_snake_snapshots("
             "snake_id TEXT PRIMARY KEY,"
             "owner_user_id TEXT NOT NULL,"
             "alive INTEGER NOT NULL,"
             "is_on_field INTEGER NOT NULL,"
             "head_x INTEGER NOT NULL,"
             "head_y INTEGER NOT NULL,"
             "direction INTEGER NOT NULL,"
             "paused INTEGER NOT NULL,"
             "length_k INTEGER NOT NULL,"
             "body_compact TEXT NOT NULL,"
             "color TEXT NOT NULL,"
             "last_event_id TEXT,"
             "created_at INTEGER NOT NULL,"
             "updated_at INTEGER NOT NULL,"
             "deleted INTEGER NOT NULL DEFAULT 0,"
             "updated_ms INTEGER NOT NULL"
             ");") &&
         Exec(
             "CREATE TABLE IF NOT EXISTS buffered_world_chunks("
             "chunk_id TEXT PRIMARY KEY,"
             "width INTEGER NOT NULL,"
             "height INTEGER NOT NULL,"
             "obstacles TEXT NOT NULL,"
             "food_state TEXT NOT NULL,"
             "version INTEGER NOT NULL,"
             "updated_at INTEGER NOT NULL,"
             "updated_ms INTEGER NOT NULL"
             ");") &&
         Exec(
             "CREATE TABLE IF NOT EXISTS buffered_period_deltas("
             "period_key TEXT PRIMARY KEY,"
             "harvested_food_delta INTEGER NOT NULL DEFAULT 0,"
             "movement_ticks_delta INTEGER NOT NULL DEFAULT 0,"
             "updated_ms INTEGER NOT NULL"
             ");") &&
         Exec(
             "CREATE TABLE IF NOT EXISTS buffered_period_user_deltas("
             "period_key TEXT NOT NULL,"
             "user_id TEXT NOT NULL,"
             "harvested_food_delta INTEGER NOT NULL DEFAULT 0,"
             "movement_ticks_delta INTEGER NOT NULL DEFAULT 0,"
             "updated_ms INTEGER NOT NULL,"
             "PRIMARY KEY(period_key,user_id)"
             ");");
}

bool BufferedSqliteStore::UpsertSnakeSnapshot(const storage::Snake& s, bool deleted) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO buffered_snake_snapshots(snake_id,owner_user_id,alive,is_on_field,head_x,head_y,direction,paused,length_k,body_compact,color,last_event_id,created_at,updated_at,deleted,updated_ms) "
      "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(snake_id) DO UPDATE SET "
      "owner_user_id=excluded.owner_user_id,alive=excluded.alive,is_on_field=excluded.is_on_field,head_x=excluded.head_x,head_y=excluded.head_y,direction=excluded.direction,paused=excluded.paused,length_k=excluded.length_k,body_compact=excluded.body_compact,color=excluded.color,last_event_id=excluded.last_event_id,created_at=excluded.created_at,updated_at=excluded.updated_at,deleted=excluded.deleted,updated_ms=excluded.updated_ms";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  int i = 1;
  bind_text(stmt, i++, s.snake_id);
  bind_text(stmt, i++, s.owner_user_id);
  sqlite3_bind_int(stmt, i++, s.alive ? 1 : 0);
  sqlite3_bind_int(stmt, i++, s.is_on_field ? 1 : 0);
  sqlite3_bind_int(stmt, i++, s.head_x);
  sqlite3_bind_int(stmt, i++, s.head_y);
  sqlite3_bind_int(stmt, i++, s.direction);
  sqlite3_bind_int(stmt, i++, s.paused ? 1 : 0);
  sqlite3_bind_int(stmt, i++, s.length_k);
  bind_text(stmt, i++, s.body_compact);
  bind_text(stmt, i++, s.color);
  bind_text(stmt, i++, s.last_event_id);
  sqlite3_bind_int64(stmt, i++, s.created_at);
  sqlite3_bind_int64(stmt, i++, s.updated_at);
  sqlite3_bind_int(stmt, i++, deleted ? 1 : 0);
  sqlite3_bind_int64(stmt, i++, now_ms());
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool BufferedSqliteStore::UpsertWorldChunk(const storage::WorldChunk& c) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO buffered_world_chunks(chunk_id,width,height,obstacles,food_state,version,updated_at,updated_ms) "
      "VALUES(?,?,?,?,?,?,?,?) "
      "ON CONFLICT(chunk_id) DO UPDATE SET "
      "width=excluded.width,height=excluded.height,obstacles=excluded.obstacles,food_state=excluded.food_state,version=excluded.version,updated_at=excluded.updated_at,updated_ms=excluded.updated_ms";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  int i = 1;
  bind_text(stmt, i++, c.chunk_id);
  sqlite3_bind_int(stmt, i++, c.width);
  sqlite3_bind_int(stmt, i++, c.height);
  bind_text(stmt, i++, c.obstacles);
  bind_text(stmt, i++, c.food_state);
  sqlite3_bind_int64(stmt, i++, c.version);
  sqlite3_bind_int64(stmt, i++, c.updated_at);
  sqlite3_bind_int64(stmt, i++, now_ms());
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool BufferedSqliteStore::UpsertPeriodDelta(const std::string& period_key, int64_t h_delta, int64_t m_delta) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO buffered_period_deltas(period_key,harvested_food_delta,movement_ticks_delta,updated_ms) VALUES(?,?,?,?) "
      "ON CONFLICT(period_key) DO UPDATE SET harvested_food_delta=harvested_food_delta+excluded.harvested_food_delta,movement_ticks_delta=movement_ticks_delta+excluded.movement_ticks_delta,updated_ms=excluded.updated_ms";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  bind_text(stmt, 1, period_key);
  sqlite3_bind_int64(stmt, 2, h_delta);
  sqlite3_bind_int64(stmt, 3, m_delta);
  sqlite3_bind_int64(stmt, 4, now_ms());
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool BufferedSqliteStore::UpsertPeriodUserDelta(const std::string& period_key,
                                                const std::string& user_id,
                                                int64_t h_delta,
                                                int64_t m_delta) {
  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "INSERT INTO buffered_period_user_deltas(period_key,user_id,harvested_food_delta,movement_ticks_delta,updated_ms) VALUES(?,?,?,?,?) "
      "ON CONFLICT(period_key,user_id) DO UPDATE SET harvested_food_delta=harvested_food_delta+excluded.harvested_food_delta,movement_ticks_delta=movement_ticks_delta+excluded.movement_ticks_delta,updated_ms=excluded.updated_ms";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
  bind_text(stmt, 1, period_key);
  bind_text(stmt, 2, user_id);
  sqlite3_bind_int64(stmt, 3, h_delta);
  sqlite3_bind_int64(stmt, 4, m_delta);
  sqlite3_bind_int64(stmt, 5, now_ms());
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool BufferedSqliteStore::BufferIntent(const PersistenceIntent& intent) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_ && !Open()) return false;

  switch (intent.type) {
    case IntentType::SnakeEventLogged: {
      if (!intent.snake_event.has_value()) return true;
      sqlite3_stmt* stmt = nullptr;
      const char* sql =
          "INSERT OR IGNORE INTO buffered_snake_events(snake_id,event_id,event_type,x,y,other_snake_id,delta_length,tick_number,world_version,created_at,created_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?)";
      if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
      int i = 1;
      bind_text(stmt, i++, intent.snake_event->snake_id);
      bind_text(stmt, i++, intent.snake_event->event_id);
      bind_text(stmt, i++, intent.snake_event->event_type);
      sqlite3_bind_int(stmt, i++, intent.snake_event->x);
      sqlite3_bind_int(stmt, i++, intent.snake_event->y);
      bind_text(stmt, i++, intent.snake_event->other_snake_id);
      sqlite3_bind_int(stmt, i++, intent.snake_event->delta_length);
      sqlite3_bind_int64(stmt, i++, static_cast<sqlite3_int64>(intent.snake_event->tick_number));
      sqlite3_bind_int64(stmt, i++, intent.snake_event->world_version);
      sqlite3_bind_int64(stmt, i++, intent.snake_event->created_at);
      sqlite3_bind_int64(stmt, i++, now_ms());
      const int rc = sqlite3_step(stmt);
      sqlite3_finalize(stmt);
      return rc == SQLITE_DONE;
    }
    case IntentType::SnakeSnapshotUpdated:
      return intent.snake_snapshot.has_value() ? UpsertSnakeSnapshot(*intent.snake_snapshot, false) : true;
    case IntentType::SnakeSnapshotDeleted:
      return intent.snake_snapshot.has_value() ? UpsertSnakeSnapshot(*intent.snake_snapshot, true) : true;
    case IntentType::WorldChunkDirty:
      return intent.world_chunk.has_value() ? UpsertWorldChunk(*intent.world_chunk) : true;
    case IntentType::UserLaborDelta:
    case IntentType::UserOutputDelta:
    case IntentType::PeriodAggregateCheckpointed:
      if (!intent.period_key.empty()) {
        if (!UpsertPeriodDelta(intent.period_key, intent.harvested_food_delta, intent.movement_ticks_delta)) return false;
        if (!intent.user_id.empty()) {
          return UpsertPeriodUserDelta(intent.period_key, intent.user_id,
                                       intent.harvested_food_delta, intent.movement_ticks_delta);
        }
      }
      return true;
    default:
      return true;
  }
}

bool BufferedSqliteStore::FlushSnakeEvents(IPermanentStore& permanent, int flush_interval_seconds) {
  const int64_t now = now_ms();
  if (now - last_events_flush_ms_ < flush_interval_seconds * 1000LL) return true;
  last_events_flush_ms_ = now;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT id,snake_id,event_id,event_type,x,y,COALESCE(other_snake_id,''),delta_length,tick_number,world_version,created_at "
      "FROM buffered_snake_events ORDER BY id LIMIT 500";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  std::vector<int64_t> ids;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PersistenceIntent intent;
    intent.type = IntentType::SnakeEventLogged;
    storage::SnakeEvent e;
    e.snake_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    e.event_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    e.event_type = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    e.x = sqlite3_column_int(stmt, 4);
    e.y = sqlite3_column_int(stmt, 5);
    e.other_snake_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
    e.delta_length = sqlite3_column_int(stmt, 7);
    e.tick_number = static_cast<uint64_t>(sqlite3_column_int64(stmt, 8));
    e.world_version = sqlite3_column_int64(stmt, 9);
    e.created_at = sqlite3_column_int64(stmt, 10);
    intent.snake_event = e;
    if (!permanent.ApplyIntent(intent)) {
      sqlite3_finalize(stmt);
      return false;
    }
    ids.push_back(sqlite3_column_int64(stmt, 0));
  }
  sqlite3_finalize(stmt);

  for (int64_t id : ids) {
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM buffered_snake_events WHERE id=?", -1, &del, nullptr) != SQLITE_OK) return false;
    sqlite3_bind_int64(del, 1, id);
    const int rc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (rc != SQLITE_DONE) return false;
  }
  return true;
}

bool BufferedSqliteStore::FlushSnakeSnapshots(IPermanentStore& permanent, int flush_interval_seconds) {
  const int64_t now = now_ms();
  if (now - last_snapshots_flush_ms_ < flush_interval_seconds * 1000LL) return true;
  last_snapshots_flush_ms_ = now;

  sqlite3_stmt* stmt = nullptr;
  const char* sql =
      "SELECT snake_id,owner_user_id,alive,is_on_field,head_x,head_y,direction,paused,length_k,body_compact,color,COALESCE(last_event_id,''),created_at,updated_at,deleted "
      "FROM buffered_snake_snapshots LIMIT 500";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  std::vector<std::string> keys;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    storage::Snake s;
    s.snake_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    s.owner_user_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    s.alive = sqlite3_column_int(stmt, 2) != 0;
    s.is_on_field = sqlite3_column_int(stmt, 3) != 0;
    s.head_x = sqlite3_column_int(stmt, 4);
    s.head_y = sqlite3_column_int(stmt, 5);
    s.direction = sqlite3_column_int(stmt, 6);
    s.paused = sqlite3_column_int(stmt, 7) != 0;
    s.length_k = sqlite3_column_int(stmt, 8);
    s.body_compact = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 9));
    s.color = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 10));
    s.last_event_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 11));
    s.created_at = sqlite3_column_int64(stmt, 12);
    s.updated_at = sqlite3_column_int64(stmt, 13);
    const bool deleted = sqlite3_column_int(stmt, 14) != 0;

    PersistenceIntent intent;
    intent.type = deleted ? IntentType::SnakeSnapshotDeleted : IntentType::SnakeSnapshotUpdated;
    intent.snake_snapshot = s;
    if (!permanent.ApplyIntent(intent)) {
      sqlite3_finalize(stmt);
      return false;
    }
    keys.push_back(s.snake_id);
  }
  sqlite3_finalize(stmt);

  for (const auto& key : keys) {
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM buffered_snake_snapshots WHERE snake_id=?", -1, &del, nullptr) != SQLITE_OK) return false;
    bind_text(del, 1, key);
    const int rc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (rc != SQLITE_DONE) return false;
  }
  return true;
}

bool BufferedSqliteStore::FlushWorldChunks(IPermanentStore& permanent, int flush_interval_seconds) {
  const int64_t now = now_ms();
  if (now - last_chunks_flush_ms_ < flush_interval_seconds * 1000LL) return true;
  last_chunks_flush_ms_ = now;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT chunk_id,width,height,obstacles,food_state,version,updated_at FROM buffered_world_chunks LIMIT 200";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  std::vector<std::string> keys;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    storage::WorldChunk c;
    c.chunk_id = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    c.width = sqlite3_column_int(stmt, 1);
    c.height = sqlite3_column_int(stmt, 2);
    c.obstacles = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
    c.food_state = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
    c.version = sqlite3_column_int64(stmt, 5);
    c.updated_at = sqlite3_column_int64(stmt, 6);

    PersistenceIntent intent;
    intent.type = IntentType::WorldChunkDirty;
    intent.world_chunk = c;
    if (!permanent.ApplyIntent(intent)) {
      sqlite3_finalize(stmt);
      return false;
    }
    keys.push_back(c.chunk_id);
  }
  sqlite3_finalize(stmt);

  for (const auto& key : keys) {
    sqlite3_stmt* del = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM buffered_world_chunks WHERE chunk_id=?", -1, &del, nullptr) != SQLITE_OK) return false;
    bind_text(del, 1, key);
    const int rc = sqlite3_step(del);
    sqlite3_finalize(del);
    if (rc != SQLITE_DONE) return false;
  }
  return true;
}

bool BufferedSqliteStore::FlushPeriodDeltas(IPermanentStore& permanent, int flush_interval_seconds) {
  const int64_t now = now_ms();
  if (now - last_period_flush_ms_ < flush_interval_seconds * 1000LL) return true;
  last_period_flush_ms_ = now;

  sqlite3_stmt* stmt = nullptr;
  const char* sql = "SELECT period_key,harvested_food_delta,movement_ticks_delta FROM buffered_period_deltas LIMIT 50";
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;

  struct PeriodRow {
    std::string period_key;
    int64_t h = 0;
    int64_t m = 0;
  };
  std::vector<PeriodRow> periods;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    PeriodRow row;
    row.period_key = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    row.h = sqlite3_column_int64(stmt, 1);
    row.m = sqlite3_column_int64(stmt, 2);
    periods.push_back(std::move(row));
  }
  sqlite3_finalize(stmt);

  for (const auto& period : periods) {
    sqlite3_stmt* ust = nullptr;
    if (sqlite3_prepare_v2(db_, "SELECT user_id,harvested_food_delta,movement_ticks_delta FROM buffered_period_user_deltas WHERE period_key=?", -1, &ust, nullptr) != SQLITE_OK) return false;
    bind_text(ust, 1, period.period_key);
    std::unordered_map<std::string, std::pair<int64_t, int64_t>> user_deltas;
    while (sqlite3_step(ust) == SQLITE_ROW) {
      const std::string user_id = reinterpret_cast<const char*>(sqlite3_column_text(ust, 0));
      user_deltas[user_id] = {sqlite3_column_int64(ust, 1), sqlite3_column_int64(ust, 2)};
    }
    sqlite3_finalize(ust);

    if (!permanent.ApplyEconomyDeltas(period.period_key, period.h, period.m, user_deltas)) return false;

    sqlite3_stmt* del1 = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM buffered_period_deltas WHERE period_key=?", -1, &del1, nullptr) != SQLITE_OK) return false;
    bind_text(del1, 1, period.period_key);
    int rc = sqlite3_step(del1);
    sqlite3_finalize(del1);
    if (rc != SQLITE_DONE) return false;

    sqlite3_stmt* del2 = nullptr;
    if (sqlite3_prepare_v2(db_, "DELETE FROM buffered_period_user_deltas WHERE period_key=?", -1, &del2, nullptr) != SQLITE_OK) return false;
    bind_text(del2, 1, period.period_key);
    rc = sqlite3_step(del2);
    sqlite3_finalize(del2);
    if (rc != SQLITE_DONE) return false;
  }
  return true;
}

bool BufferedSqliteStore::FlushDue(IPermanentStore& permanent,
                                   const PersistencePolicyRegistry&,
                                   int flush_chunks_seconds,
                                   int flush_snapshots_seconds,
                                   int flush_period_deltas_seconds) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) return false;

  if (!FlushWorldChunks(permanent, flush_chunks_seconds)) return false;
  if (!FlushSnakeSnapshots(permanent, flush_snapshots_seconds)) return false;
  if (!FlushPeriodDeltas(permanent, flush_period_deltas_seconds)) return false;
  // Keep snake events cheap by flushing less frequently via snapshot cadence default.
  if (!FlushSnakeEvents(permanent, std::max(15, flush_snapshots_seconds))) return false;
  return true;
}

bool BufferedSqliteStore::Cleanup(int retention_hours, int max_mb) {
  std::lock_guard<std::mutex> lock(mu_);
  if (!db_) return false;

  const int64_t cutoff_ms = now_ms() - (static_cast<int64_t>(retention_hours) * 3600LL * 1000LL);
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db_, "DELETE FROM buffered_snake_events WHERE created_ms < ?", -1, &stmt, nullptr) == SQLITE_OK) {
    sqlite3_bind_int64(stmt, 1, cutoff_ms);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
  }

  try {
    const auto sz = std::filesystem::exists(path_) ? static_cast<int64_t>(std::filesystem::file_size(path_)) : 0;
    const int64_t max_bytes = static_cast<int64_t>(max_mb) * 1024LL * 1024LL;
    if (max_bytes > 0 && sz > max_bytes) {
      // Trim oldest events aggressively when DB exceeds profile budget.
      Exec("DELETE FROM buffered_snake_events WHERE id IN (SELECT id FROM buffered_snake_events ORDER BY id ASC LIMIT 5000)");
    }
  } catch (...) {
  }

  sqlite3_exec(db_, "PRAGMA wal_checkpoint(TRUNCATE);", nullptr, nullptr, nullptr);
  return true;
}

}  // namespace persistence
