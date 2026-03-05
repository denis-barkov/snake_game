#include "compute.h"

#include <algorithm>

namespace economy_engine {

CapitalAggregate AggregateProductiveCapital(const std::vector<storage::Snake>& snakes) {
  CapitalAggregate out;
  for (const auto& s : snakes) {
    if (!s.alive || !s.is_on_field) continue;
    const int64_t k = std::max<int64_t>(0, s.length_k);
    out.snake_capital += k;
    out.total_capital += k;
    out.user_total_capital[s.owner_user_id] += k;
  }
  return out;
}

economy::EconomySnapshot ComputeGlobal(const economy::EconomyPeriodRaw& raw,
                                       const std::optional<economy::EconomySnapshot>& prev,
                                       int64_t users_sum_balance,
                                       int64_t treasury_balance) {
  return economy::ComputeGlobal(raw, prev ? &*prev : nullptr, users_sum_balance, treasury_balance);
}

economy::EconomyUserSnapshot ComputeUser(const economy::EconomyPeriodRaw& raw_user,
                                         const std::optional<economy::EconomyUserSnapshot>& prev_user,
                                         int64_t user_balance,
                                         int64_t global_y,
                                         const std::string& period_id,
                                         const std::string& user_id) {
  return economy::ComputeUser(raw_user, prev_user ? &*prev_user : nullptr, user_balance, global_y, period_id, user_id);
}

}  // namespace economy_engine

