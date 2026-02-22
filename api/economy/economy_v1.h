#pragma once

#include <cstdint>
#include <string>

#include "../storage/models.h"

namespace economy {

struct EconomyInputs {
  storage::EconomyParams params;
  int64_t sum_mi = 0;        // ΣM_i
  int64_t m_g = 0;           // M_G
  int64_t delta_m_buy = 0;   // ΔM_buy
  int64_t delta_m_issue = 0; // ΔM_issue
  int64_t cap_delta_m = 0;   // Cap_ΔM
  int64_t k_snakes = 0;      // K_snakes
  int64_t delta_k_obs = 0;   // ΔK_obs
};

struct EconomyState {
  std::string period_key;
  int64_t sum_mi = 0;
  int64_t m_g = 0;
  int64_t m = 0;        // Money supply
  int64_t delta_m = 0;  // Money growth for current period
  int64_t k = 0;        // Effective capital
  double y = 0.0;       // Output
  double p = 0.0;       // Price index
  double pi = 0.0;      // Inflation
  int64_t a_world = 0;  // Implied world area
  int64_t m_white = 0;  // Free space
  double p_clamped = 0.0;
};

EconomyState ComputeEconomyV1(const EconomyInputs& in, const std::string& period_key);

}  // namespace economy
