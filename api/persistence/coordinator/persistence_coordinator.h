#pragma once

#include <memory>

#include "../flush/flush_scheduler.h"
#include "../interfaces/interfaces.h"
#include "../router/persistence_router.h"

namespace persistence {

struct CoordinatorConfig {
  int sqlite_retention_hours = 72;
  int sqlite_max_mb = 256;
  int flush_chunks_seconds = 2;
  int flush_snapshots_seconds = 10;
  int flush_period_deltas_seconds = 10;
};

class PersistenceCoordinator final : public IPersistenceCoordinator {
 public:
  PersistenceCoordinator(CoordinatorConfig cfg,
                         IRuntimeStore& runtime_store,
                         IBufferedStore& buffered_store,
                         IPermanentStore& permanent_store,
                         const PersistencePolicyRegistry& registry,
                         IPersistenceRouter& router);
  ~PersistenceCoordinator() override;

  bool Emit(const PersistenceIntent& intent) override;
  void FlushNow() override;
  void Start() override;
  void Stop() override;

 private:
  void TickFlush();

  CoordinatorConfig cfg_;
  IRuntimeStore& runtime_store_;
  IBufferedStore& buffered_store_;
  IPermanentStore& permanent_store_;
  const PersistencePolicyRegistry& registry_;
  IPersistenceRouter& router_;
  std::unique_ptr<FlushScheduler> scheduler_;
};

}  // namespace persistence
