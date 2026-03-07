#include "persistence_coordinator.h"

namespace persistence {

PersistenceCoordinator::PersistenceCoordinator(CoordinatorConfig cfg,
                                               IRuntimeStore& runtime_store,
                                               IBufferedStore& buffered_store,
                                               IPermanentStore& permanent_store,
                                               const PersistencePolicyRegistry& registry,
                                               IPersistenceRouter& router)
    : cfg_(cfg),
      runtime_store_(runtime_store),
      buffered_store_(buffered_store),
      permanent_store_(permanent_store),
      registry_(registry),
      router_(router) {
  scheduler_ = std::make_unique<FlushScheduler>([this] { TickFlush(); });
}

PersistenceCoordinator::~PersistenceCoordinator() {
  Stop();
}

bool PersistenceCoordinator::Emit(const PersistenceIntent& intent) {
  runtime_store_.RecordIntent(intent);

  const auto policy = router_.Route(intent);
  switch (policy.write_mode) {
    case WriteMode::RuntimeOnly:
      return true;
    case WriteMode::BufferedOnly:
      return buffered_store_.BufferIntent(intent);
    case WriteMode::DirectPermanent:
      return permanent_store_.ApplyIntent(intent);
    case WriteMode::BufferedThenFlush:
      return buffered_store_.BufferIntent(intent);
    case WriteMode::DirectAndBuffer: {
      const bool ok_perm = permanent_store_.ApplyIntent(intent);
      const bool ok_buf = buffered_store_.BufferIntent(intent);
      return ok_perm && ok_buf;
    }
  }
  return true;
}

void PersistenceCoordinator::TickFlush() {
  (void)buffered_store_.FlushDue(permanent_store_, registry_,
                                 cfg_.flush_chunks_seconds,
                                 cfg_.flush_snapshots_seconds,
                                 cfg_.flush_period_deltas_seconds);
  (void)buffered_store_.Cleanup(cfg_.sqlite_retention_hours, cfg_.sqlite_max_mb);
}

void PersistenceCoordinator::FlushNow() {
  TickFlush();
}

void PersistenceCoordinator::Start() {
  if (scheduler_) scheduler_->Start();
}

void PersistenceCoordinator::Stop() {
  if (scheduler_) scheduler_->Stop();
  FlushNow();
}

}  // namespace persistence
