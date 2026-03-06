#pragma once

#include <cstdint>
#include <mutex>
#include <string>

#include <sqlite3.h>

#include "../../interfaces/interfaces.h"

namespace persistence {

class BufferedSqliteStore final : public IBufferedStore {
 public:
  explicit BufferedSqliteStore(std::string sqlite_path);
  ~BufferedSqliteStore() override;

  bool BufferIntent(const PersistenceIntent& intent) override;
  bool FlushDue(IPermanentStore& permanent,
                const PersistencePolicyRegistry& registry,
                int flush_chunks_seconds,
                int flush_snapshots_seconds,
                int flush_period_deltas_seconds) override;
  bool Cleanup(int retention_hours, int max_mb) override;

 private:
  bool Open();
  bool EnsureSchema();
  bool Exec(const std::string& sql);
  bool UpsertSnakeSnapshot(const storage::Snake& s, bool deleted);
  bool UpsertWorldChunk(const storage::WorldChunk& c);
  bool UpsertPeriodDelta(const std::string& period_key, int64_t h_delta, int64_t m_delta);
  bool UpsertPeriodUserDelta(const std::string& period_key, const std::string& user_id, int64_t h_delta, int64_t m_delta);

  bool FlushSnakeEvents(IPermanentStore& permanent, int flush_interval_seconds);
  bool FlushSnakeSnapshots(IPermanentStore& permanent, int flush_interval_seconds);
  bool FlushWorldChunks(IPermanentStore& permanent, int flush_interval_seconds);
  bool FlushPeriodDeltas(IPermanentStore& permanent, int flush_interval_seconds);

  std::string path_;
  sqlite3* db_ = nullptr;
  std::mutex mu_;

  int64_t last_events_flush_ms_ = 0;
  int64_t last_snapshots_flush_ms_ = 0;
  int64_t last_chunks_flush_ms_ = 0;
  int64_t last_period_flush_ms_ = 0;
};

}  // namespace persistence
