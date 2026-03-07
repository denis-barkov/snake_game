#pragma once

#include <optional>
#include <string>
#include <vector>

#include "types.h"

namespace economy_engine {

CapitalAggregate AggregateProductiveCapital(const std::vector<storage::Snake>& snakes);

economy::EconomySnapshot ComputeGlobal(const economy::EconomyPeriodRaw& raw,
                                       const std::optional<economy::EconomySnapshot>& prev,
                                       int64_t users_sum_balance,
                                       int64_t treasury_balance);

economy::EconomyUserSnapshot ComputeUser(const economy::EconomyPeriodRaw& raw_user,
                                         const std::optional<economy::EconomyUserSnapshot>& prev_user,
                                         int64_t user_balance,
                                         int64_t global_y,
                                         const std::string& period_id,
                                         const std::string& user_id);

}  // namespace economy_engine

