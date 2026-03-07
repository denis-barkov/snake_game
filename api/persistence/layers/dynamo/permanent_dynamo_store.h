#pragma once

#include <unordered_map>

#include "../../../storage/storage.h"
#include "../../interfaces/interfaces.h"

namespace persistence {

class PermanentDynamoStore final : public IPermanentStore {
 public:
  explicit PermanentDynamoStore(storage::IStorage& storage) : storage_(storage) {}

  bool ApplyIntent(const PersistenceIntent& intent) override;
  bool ApplyEconomyDeltas(const std::string& period_key,
                          int64_t harvested_food_delta,
                          int64_t movement_ticks_delta,
                          const std::unordered_map<std::string, std::pair<int64_t, int64_t>>& user_deltas) override;

 private:
  storage::IStorage& storage_;
};

}  // namespace persistence
