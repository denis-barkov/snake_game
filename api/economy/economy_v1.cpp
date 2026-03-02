#include "economy_v1.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <sstream>

namespace economy {
namespace {

double Clamp(double value, double min_v, double max_v) {
  return std::max(min_v, std::min(max_v, value));
}

double SafePow(double base, double exp) {
  if (base <= 0.0) return 1.0;
  return std::pow(base, exp);
}

std::string FormatYmd(const std::tm& tm_local) {
  char buf[16];
  if (std::strftime(buf, sizeof(buf), "%Y%m%d", &tm_local) == 0) return "19700101";
  return buf;
}

}  // namespace

EconomySnapshot ComputeGlobal(const EconomyPeriodRaw& raw,
                              const EconomySnapshot* prev,
                              int64_t users_sum_balance,
                              int64_t treasury_balance) {
  EconomySnapshot out;
  out.y = std::max<int64_t>(0, raw.harvested_food);
  out.k = std::max<int64_t>(0, raw.deployed_cells);
  out.l = std::max<int64_t>(0, raw.movement_ticks);
  out.m = std::max<int64_t>(0, users_sum_balance + treasury_balance);
  out.treasury_balance = treasury_balance;
  out.p = static_cast<double>(out.m) / static_cast<double>(std::max<int64_t>(out.y, 1));

  if (!prev) {
    out.alpha = 0.5;
    out.alpha_bootstrap = true;
    out.pi = 0.0;
  } else {
    const int64_t d_y = out.y - prev->y;
    const int64_t d_k = out.k - prev->k;
    const double mpk = d_k > 0 ? (static_cast<double>(d_y) / static_cast<double>(d_k)) : 0.0;
    out.alpha = Clamp((mpk * static_cast<double>(out.k)) / static_cast<double>(std::max<int64_t>(out.y, 1)), 0.0, 1.0);
    const double prev_p = std::max(prev->p, 1e-9);
    out.pi = (out.p - prev->p) / prev_p;
    out.alpha_bootstrap = false;
  }

  const double k_term = SafePow(static_cast<double>(std::max<int64_t>(out.k, 1)), out.alpha);
  const double l_term = SafePow(static_cast<double>(std::max<int64_t>(out.l, 1)), 1.0 - out.alpha);
  const double denom = std::max(1e-9, k_term * l_term);
  out.a = static_cast<double>(out.y) / denom;
  return out;
}

EconomyUserSnapshot ComputeUser(const EconomyPeriodRaw& raw_user,
                                const EconomyUserSnapshot* prev_user,
                                int64_t user_balance,
                                int64_t global_y,
                                const std::string& period_id,
                                const std::string& user_id) {
  EconomyUserSnapshot out;
  out.period_id = period_id;
  out.user_id = user_id;
  out.y_u = std::max<int64_t>(0, raw_user.harvested_food);
  out.k_u = std::max<int64_t>(0, raw_user.deployed_cells);
  out.l_u = std::max<int64_t>(0, raw_user.movement_ticks);
  out.storage_balance = user_balance;
  out.market_share = static_cast<double>(out.y_u) / static_cast<double>(std::max<int64_t>(global_y, 1));

  if (!prev_user) {
    out.alpha_u = 0.5;
    out.alpha_bootstrap = true;
  } else {
    const int64_t d_y = out.y_u - prev_user->y_u;
    const int64_t d_k = out.k_u - prev_user->k_u;
    const double mpk = d_k > 0 ? (static_cast<double>(d_y) / static_cast<double>(d_k)) : 0.0;
    out.alpha_u = Clamp((mpk * static_cast<double>(out.k_u)) / static_cast<double>(std::max<int64_t>(out.y_u, 1)), 0.0, 1.0);
    out.alpha_bootstrap = false;
  }

  const double k_term = SafePow(static_cast<double>(std::max<int64_t>(out.k_u, 1)), out.alpha_u);
  const double l_term = SafePow(static_cast<double>(std::max<int64_t>(out.l_u, 1)), 1.0 - out.alpha_u);
  out.a_u = static_cast<double>(out.y_u) / std::max(1e-9, k_term * l_term);
  return out;
}

PeriodState CurrentPeriodState(std::time_t now_utc, const PeriodConfig& cfg) {
  PeriodState out;
  const int period_seconds = std::max(60, cfg.period_seconds);

  if (cfg.align_mode == "midnight") {
    std::tm local_tm{};
#if defined(_WIN32)
    localtime_s(&local_tm, &now_utc);
#else
    localtime_r(&now_utc, &local_tm);
#endif
    out.period_id = FormatYmd(local_tm);
    std::tm next_tm = local_tm;
    next_tm.tm_hour = 0;
    next_tm.tm_min = 0;
    next_tm.tm_sec = 0;
    next_tm.tm_mday += 1;
    const std::time_t next_time = std::mktime(&next_tm);
    out.ends_in_seconds = std::max<int64_t>(0, static_cast<int64_t>(next_time - now_utc));
    return out;
  }

  const int64_t epoch = static_cast<int64_t>(now_utc);
  const int64_t idx = epoch / period_seconds;
  out.period_id = "p" + std::to_string(idx);
  out.ends_in_seconds = period_seconds - (epoch % period_seconds);
  return out;
}

EconomyState ComputeEconomyV1(const EconomyInputs& in, const std::string& period_key) {
  EconomyState out;
  out.period_key = period_key;
  out.sum_mi = in.sum_mi;
  out.m_g = in.m_g;
  out.m = in.sum_mi + in.m_g;
  out.delta_m = std::min(in.cap_delta_m, in.delta_m_issue) + in.delta_m_buy;
  out.k = in.k_snakes + in.delta_k_obs;
  out.y = in.params.a_productivity * static_cast<double>(out.k);
  out.p = static_cast<double>(out.m) * in.params.v_velocity / std::max(1.0, out.y);
  out.p_clamped = Clamp(out.p, 0.2, 5.0);
  out.pi = static_cast<double>(out.delta_m) / static_cast<double>(std::max<int64_t>(1, out.m));
  out.a_world = static_cast<int64_t>(in.params.k_land) * out.m;
  out.m_white = std::max<int64_t>(0, out.a_world - out.k);
  return out;
}

}  // namespace economy

