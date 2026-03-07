#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "../economy/economy_v1.h"
#include "../storage/models.h"

namespace economy_engine {

struct CapitalAggregate {
  int64_t snake_capital = 0;
  int64_t machine_capital = 0;
  int64_t total_capital = 0;
  std::unordered_map<std::string, int64_t> user_total_capital;
};

struct EconomyComputed {
  economy::EconomySnapshot global;
  std::unordered_map<std::string, economy::EconomyUserSnapshot> users;
};

}  // namespace economy_engine

