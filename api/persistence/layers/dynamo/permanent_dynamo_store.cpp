#include "permanent_dynamo_store.h"

namespace persistence {

bool PermanentDynamoStore::ApplyIntent(const PersistenceIntent& intent) {
  switch (intent.type) {
    case IntentType::UserBalanceChanged:
      if (intent.user_id.empty() || intent.delta_i64 == 0) return true;
      return storage_.IncrementUserBalance(intent.user_id, intent.delta_i64);
    case IntentType::SnakeCreated:
    case IntentType::SnakeExtended:
    case IntentType::SnakeSnapshotUpdated:
      if (!intent.snake_snapshot.has_value()) return true;
      return storage_.PutSnake(*intent.snake_snapshot);
    case IntentType::SnakeSnapshotDeleted:
      if (!intent.snake_snapshot.has_value()) return true;
      return storage_.DeleteSnake(intent.snake_snapshot->snake_id);
    case IntentType::SnakeEventLogged:
      if (!intent.snake_event.has_value()) return true;
      return storage_.AppendSnakeEvent(*intent.snake_event);
    case IntentType::WorldChunkDirty:
      if (!intent.world_chunk.has_value()) return true;
      return storage_.PutWorldChunk(*intent.world_chunk);
    case IntentType::SettingsUpdated:
      return true;
    case IntentType::PeriodAggregateFinalized:
      if (!intent.finalized_period.has_value()) return true;
      return storage_.PutEconomyPeriod(*intent.finalized_period);
    case IntentType::SnakeDeathSettled:
      if (intent.delta_i64 != 0) return storage_.IncrementSystemReserve(intent.delta_i64);
      return true;
    case IntentType::UserLaborDelta:
    case IntentType::UserOutputDelta:
    case IntentType::PeriodAggregateCheckpointed:
      // Aggregated through ApplyEconomyDeltas for better coalescing.
      return true;
    default:
      return true;
  }
}

bool PermanentDynamoStore::ApplyEconomyDeltas(
    const std::string& period_key,
    int64_t harvested_food_delta,
    int64_t movement_ticks_delta,
    const std::unordered_map<std::string, std::pair<int64_t, int64_t>>& user_deltas) {
  if (period_key.empty()) return true;
  if (!storage_.IncrementEconomyPeriodRaw(period_key, harvested_food_delta, movement_ticks_delta)) return false;
  for (const auto& kv : user_deltas) {
    if (!storage_.IncrementEconomyPeriodUserRaw(period_key, kv.first, kv.second.first, kv.second.second)) {
      return false;
    }
  }
  return true;
}

}  // namespace persistence
