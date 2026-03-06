#pragma once

#include <cstdint>
#include <string>

#include "../intents/persistence_intent.h"

namespace persistence {

enum class TargetLayer {
  Runtime,
  Buffered,
  Permanent,
  None,
};

enum class WriteMode {
  RuntimeOnly,
  BufferedOnly,
  DirectPermanent,
  BufferedThenFlush,
  DirectAndBuffer,
};

struct PersistencePolicy {
  TargetLayer primary_target_layer = TargetLayer::Runtime;
  TargetLayer secondary_target_layer = TargetLayer::None;
  WriteMode write_mode = WriteMode::RuntimeOnly;

  bool runtime_only = false;
  bool buffered_only = false;
  bool direct_permanent = false;
  bool buffered_then_flush = false;
  bool direct_and_buffer = false;

  int flush_interval_seconds = 10;
  bool period_finalize_action = false;
  bool critical_bypass = false;
  int retention_hours = 24;
  std::string coalescing_key;
};

struct PersistencePolicyRegistry {
  virtual ~PersistencePolicyRegistry() = default;
  virtual PersistencePolicy Resolve(IntentType type) const = 0;
};

}  // namespace persistence
