#include "economy_v1.h"

#include <algorithm>

namespace economy {
namespace {

double clamp(double value, double min_v, double max_v) {
  return std::max(min_v, std::min(max_v, value));
}

}  // namespace

EconomyState ComputeEconomyV1(const EconomyInputs& in, const std::string& period_key) {
  EconomyState out;
  out.period_key = period_key;
  out.sum_mi = in.sum_mi;
  out.m_g = in.m_g;

  // v1 formulas are deterministic and side-effect free.
  out.m = in.sum_mi + in.m_g;
  out.delta_m = std::min(in.cap_delta_m, in.delta_m_issue) + in.delta_m_buy;
  out.k = in.k_snakes + in.delta_k_obs;
  out.y = in.params.a_productivity * static_cast<double>(out.k);

  const double denom_y = std::max(1.0, out.y);
  out.p = (static_cast<double>(out.m) * in.params.v_velocity) / denom_y;
  out.p_clamped = clamp(out.p, 0.2, 5.0);

  const double denom_m = static_cast<double>(std::max<int64_t>(1, out.m));
  out.pi = static_cast<double>(out.delta_m) / denom_m;

  out.a_world = static_cast<int64_t>(in.params.k_land) * out.m;
  out.m_white = std::max<int64_t>(0, out.a_world - out.k);
  return out;
}

}  // namespace economy
