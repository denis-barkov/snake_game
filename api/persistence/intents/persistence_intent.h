#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "../../storage/models.h"

namespace persistence {

enum class IntentType {
  UserBalanceChanged,
  SnakeCreated,
  SnakeExtended,
  SnakeDeathSettled,
  SnakeEventLogged,
  UserLaborDelta,
  UserOutputDelta,
  WorldChunkDirty,
  SnakeSnapshotUpdated,
  SnakeSnapshotDeleted,
  SettingsUpdated,
  PeriodAggregateCheckpointed,
  PeriodAggregateFinalized,
};

struct PersistenceIntent {
  IntentType type = IntentType::SnakeEventLogged;
  std::string op_id;  // For idempotency/dedup on critical intents.

  // Generic payload fields used by specific intent types.
  std::string user_id;
  int64_t delta_i64 = 0;

  std::optional<storage::SnakeEvent> snake_event;
  std::optional<storage::Snake> snake_snapshot;
  std::optional<storage::WorldChunk> world_chunk;

  // Economy per-period deltas.
  std::string period_key;
  int64_t harvested_food_delta = 0;
  int64_t movement_ticks_delta = 0;

  // Period finalization payload.
  std::optional<storage::EconomyPeriod> finalized_period;
};

}  // namespace persistence
