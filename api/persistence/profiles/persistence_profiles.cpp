#include "persistence_profiles.h"

#include <algorithm>

namespace persistence {
namespace {

PersistencePolicy RuntimeOnly() {
  PersistencePolicy p;
  p.primary_target_layer = TargetLayer::Runtime;
  p.write_mode = WriteMode::RuntimeOnly;
  p.runtime_only = true;
  return p;
}

PersistencePolicy Buffered(int flush_s, int retention_h, const std::string& key = "") {
  PersistencePolicy p;
  p.primary_target_layer = TargetLayer::Buffered;
  p.write_mode = WriteMode::BufferedThenFlush;
  p.buffered_then_flush = true;
  p.flush_interval_seconds = flush_s;
  p.retention_hours = retention_h;
  p.coalescing_key = key;
  return p;
}

PersistencePolicy BufferedOnly(int retention_h, const std::string& key = "") {
  PersistencePolicy p;
  p.primary_target_layer = TargetLayer::Buffered;
  p.write_mode = WriteMode::BufferedOnly;
  p.buffered_only = true;
  p.retention_hours = retention_h;
  p.coalescing_key = key;
  return p;
}

PersistencePolicy Direct(bool critical = false) {
  PersistencePolicy p;
  p.primary_target_layer = TargetLayer::Permanent;
  p.write_mode = WriteMode::DirectPermanent;
  p.direct_permanent = true;
  p.critical_bypass = critical;
  return p;
}

PersistencePolicy DirectAndBuffer(int flush_s, int retention_h, const std::string& key = "") {
  PersistencePolicy p;
  p.primary_target_layer = TargetLayer::Permanent;
  p.secondary_target_layer = TargetLayer::Buffered;
  p.write_mode = WriteMode::DirectAndBuffer;
  p.direct_and_buffer = true;
  p.flush_interval_seconds = flush_s;
  p.retention_hours = retention_h;
  p.coalescing_key = key;
  return p;
}

}  // namespace

ProfilePolicyRegistry::ProfilePolicyRegistry(std::string profile_name,
                                             int default_retention_hours)
    : profile_name_(std::move(profile_name)) {
  if (profile_name_.empty()) profile_name_ = "minimal";

  // Default policies shared by all profiles.
  policies_[IntentType::UserBalanceChanged] = Direct(true);
  policies_[IntentType::SnakeCreated] = Direct(true);
  policies_[IntentType::SnakeExtended] = Direct(true);
  policies_[IntentType::SnakeDeathSettled] = Direct(true);
  policies_[IntentType::PeriodAggregateFinalized] = Direct(true);
  policies_[IntentType::SettingsUpdated] = Direct(true);

  policies_[IntentType::SnakeEventLogged] = BufferedOnly(default_retention_hours, "snake_event");
  policies_[IntentType::UserLaborDelta] = Buffered(10, default_retention_hours, "period");
  policies_[IntentType::UserOutputDelta] = Buffered(10, default_retention_hours, "period");
  policies_[IntentType::WorldChunkDirty] = Buffered(2, default_retention_hours, "chunk");
  policies_[IntentType::SnakeSnapshotUpdated] = Buffered(10, default_retention_hours, "snake");
  policies_[IntentType::SnakeSnapshotDeleted] = Buffered(10, default_retention_hours, "snake");
  policies_[IntentType::PeriodAggregateCheckpointed] = Buffered(10, default_retention_hours, "period");

  if (profile_name_ == "standard") {
    policies_[IntentType::SnakeEventLogged] = Buffered(30, default_retention_hours, "snake_event");
    policies_[IntentType::SnakeSnapshotUpdated] = Buffered(5, default_retention_hours, "snake");
    policies_[IntentType::SnakeSnapshotDeleted] = Buffered(5, default_retention_hours, "snake");
    policies_[IntentType::UserLaborDelta] = Buffered(5, default_retention_hours, "period");
    policies_[IntentType::UserOutputDelta] = Buffered(5, default_retention_hours, "period");
  } else if (profile_name_ == "payments_safe") {
    policies_[IntentType::SnakeEventLogged] = Buffered(15, default_retention_hours, "snake_event");
    policies_[IntentType::SnakeSnapshotUpdated] = Buffered(5, default_retention_hours, "snake");
    policies_[IntentType::SnakeSnapshotDeleted] = Buffered(5, default_retention_hours, "snake");
    policies_[IntentType::UserLaborDelta] = Buffered(3, default_retention_hours, "period");
    policies_[IntentType::UserOutputDelta] = Buffered(3, default_retention_hours, "period");
    policies_[IntentType::PeriodAggregateCheckpointed] = DirectAndBuffer(3, default_retention_hours, "period");
  } else if (profile_name_ == "strict") {
    policies_[IntentType::SnakeEventLogged] = Buffered(5, default_retention_hours, "snake_event");
    policies_[IntentType::SnakeSnapshotUpdated] = DirectAndBuffer(2, default_retention_hours, "snake");
    policies_[IntentType::SnakeSnapshotDeleted] = DirectAndBuffer(2, default_retention_hours, "snake");
    policies_[IntentType::WorldChunkDirty] = DirectAndBuffer(1, default_retention_hours, "chunk");
    policies_[IntentType::UserLaborDelta] = Buffered(2, default_retention_hours, "period");
    policies_[IntentType::UserOutputDelta] = Buffered(2, default_retention_hours, "period");
    policies_[IntentType::PeriodAggregateCheckpointed] = DirectAndBuffer(2, default_retention_hours, "period");
  }
}

PersistencePolicy ProfilePolicyRegistry::Resolve(IntentType type) const {
  auto it = policies_.find(type);
  if (it != policies_.end()) return it->second;
  return RuntimeOnly();
}

}  // namespace persistence
