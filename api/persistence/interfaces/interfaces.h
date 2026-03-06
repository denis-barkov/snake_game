#pragma once

#include <unordered_map>
#include <vector>

#include "../intents/persistence_intent.h"
#include "../policy/persistence_policy.h"

namespace persistence {

struct IRuntimeStore {
  virtual ~IRuntimeStore() = default;
  virtual void RecordIntent(const PersistenceIntent& intent) = 0;
};

struct IPermanentStore {
  virtual ~IPermanentStore() = default;
  virtual bool ApplyIntent(const PersistenceIntent& intent) = 0;
  virtual bool ApplyEconomyDeltas(const std::string& period_key,
                                  int64_t harvested_food_delta,
                                  int64_t movement_ticks_delta,
                                  const std::unordered_map<std::string, std::pair<int64_t, int64_t>>& user_deltas) = 0;
};

struct IBufferedStore {
  virtual ~IBufferedStore() = default;
  virtual bool BufferIntent(const PersistenceIntent& intent) = 0;
  virtual bool FlushDue(IPermanentStore& permanent,
                        const PersistencePolicyRegistry& registry,
                        int flush_chunks_seconds,
                        int flush_snapshots_seconds,
                        int flush_period_deltas_seconds) = 0;
  virtual bool Cleanup(int retention_hours, int max_mb) = 0;
};

struct IPersistenceRouter {
  virtual ~IPersistenceRouter() = default;
  virtual PersistencePolicy Route(const PersistenceIntent& intent) const = 0;
};

struct IFlushScheduler {
  virtual ~IFlushScheduler() = default;
  virtual void Start() = 0;
  virtual void Stop() = 0;
};

struct IPersistenceCoordinator {
  virtual ~IPersistenceCoordinator() = default;
  virtual bool Emit(const PersistenceIntent& intent) = 0;
  virtual void FlushNow() = 0;
  virtual void Start() = 0;
  virtual void Stop() = 0;
};

}  // namespace persistence
