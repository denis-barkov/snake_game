#pragma once

#include <cstdint>
#include <ctime>
#include <string>

#include "../storage/models.h"

namespace economy {

struct EconomyPeriodRaw {
  int64_t harvested_food = 0;   // Y
  int64_t movement_ticks = 0;   // L
  int64_t deployed_cells = 0;   // K
};

struct EconomySnapshot {
  std::string period_id;
  int64_t y = 0;
  int64_t k = 0;
  int64_t l = 0;
  double alpha = 0.5;
  double a = 0.0;
  int64_t m = 0;
  double p = 0.0;
  double pi = 0.0;
  int64_t treasury_balance = 0;
  bool alpha_bootstrap = false;
  std::string snapshot_status = "live_unfinalized";
  int64_t period_ends_in_seconds = 0;
};

struct EconomyUserSnapshot {
  std::string period_id;
  std::string user_id;
  int64_t y_u = 0;
  int64_t k_u = 0;
  int64_t l_u = 0;
  double alpha_u = 0.5;
  double a_u = 0.0;
  double market_share = 0.0;
  int64_t storage_balance = 0;
  bool alpha_bootstrap = false;
};

struct PeriodConfig {
  int period_seconds = 300;
  std::string align_mode = "rolling";  // rolling|midnight
};

struct PeriodState {
  std::string period_id;
  int64_t ends_in_seconds = 0;
};

EconomySnapshot ComputeGlobal(const EconomyPeriodRaw& raw,
                              const EconomySnapshot* prev,
                              int64_t users_sum_balance,
                              int64_t treasury_balance);

EconomyUserSnapshot ComputeUser(const EconomyPeriodRaw& raw_user,
                                const EconomyUserSnapshot* prev_user,
                                int64_t user_balance,
                                int64_t global_y,
                                const std::string& period_id,
                                const std::string& user_id);

PeriodState CurrentPeriodState(std::time_t now_utc, const PeriodConfig& cfg);

// Backward-compatible wrapper kept for existing call sites during migration.
struct EconomyInputs {
  storage::EconomyParams params;
  int64_t sum_mi = 0;
  int64_t m_g = 0;
  int64_t delta_m_buy = 0;
  int64_t delta_m_issue = 0;
  int64_t cap_delta_m = 0;
  int64_t k_snakes = 0;
  int64_t delta_k_obs = 0;
};

struct EconomyState {
  std::string period_key;
  int64_t sum_mi = 0;
  int64_t m_g = 0;
  int64_t m = 0;
  int64_t delta_m = 0;
  int64_t k = 0;
  double y = 0.0;
  double p = 0.0;
  double pi = 0.0;
  int64_t a_world = 0;
  int64_t m_white = 0;
  double p_clamped = 0.0;
};

EconomyState ComputeEconomyV1(const EconomyInputs& in, const std::string& period_key);

}  // namespace economy
