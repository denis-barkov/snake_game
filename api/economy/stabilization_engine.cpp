#include "stabilization_engine.h"

#include <algorithm>
#include <cmath>
#include <unordered_set>
#include <utility>

namespace economy {

namespace {

double safe_ratio(int64_t num, int64_t den) {
  return static_cast<double>(num) / static_cast<double>(std::max<int64_t>(den, 1));
}

}  // namespace

StabilizationEngine::StabilizationEngine(StabilizationConfig cfg) : cfg_(std::move(cfg)) {}

int64_t StabilizationEngine::ComputeOccupiedSnakeCells(const world::WorldSnapshot& world) {
  std::unordered_set<uint64_t> occupied;
  occupied.reserve(world.snakes.size() * 8);

  for (const auto& s : world.snakes) {
    if (!s.alive) continue;
    for (const auto& seg : s.body) {
      if (seg.x < 0 || seg.y < 0 || seg.x >= world.w || seg.y >= world.h) continue;
      const uint64_t key = (static_cast<uint64_t>(static_cast<uint32_t>(seg.y)) << 32) |
                           static_cast<uint32_t>(seg.x);
      occupied.insert(key);
    }
  }
  return static_cast<int64_t>(occupied.size());
}

StabilizationDerived StabilizationEngine::Derive(int64_t money_supply,
                                                 int64_t k_lend,
                                                 int64_t deployed_capital,
                                                 int64_t field_size,
                                                 int64_t free_space_on_field) const {
  StabilizationDerived out;
  out.money_supply = std::max<int64_t>(0, money_supply);
  out.k_lend = std::max<int64_t>(1, k_lend);
  out.deployed_capital = std::max<int64_t>(0, deployed_capital);
  out.field_size = std::max<int64_t>(0, field_size);
  out.free_space_on_field = std::max<int64_t>(0, free_space_on_field);
  out.total_theoretical_space = out.k_lend * out.money_supply;
  out.treasury_white_space =
      std::max<int64_t>(0, out.total_theoretical_space - out.deployed_capital - out.free_space_on_field);
  out.spatial_ratio_r = safe_ratio(out.free_space_on_field, out.deployed_capital);
  out.lcr = safe_ratio(out.treasury_white_space, out.deployed_capital);
  return out;
}

FastCheckDecision StabilizationEngine::EvaluateFastSpatialCheck(const StabilizationDerived& d) {
  FastCheckDecision out;
  if (state_.expansion_recent_checks_remaining > 0) {
    state_.expansion_recent_checks_remaining -= 1;
  }
  if (!cfg_.auto_expansion_enabled) {
    state_.liquidity_constraint_mode_active = false;
    return out;
  }

  if (d.spatial_ratio_r >= cfg_.auto_expansion_trigger_ratio) {
    state_.liquidity_constraint_mode_active = false;
    return out;
  }

  out.triggered = true;
  const int64_t required_free_space =
      static_cast<int64_t>(std::ceil(cfg_.target_spatial_ratio * static_cast<double>(std::max<int64_t>(d.deployed_capital, 0))));
  out.required_expansion_cells = std::max<int64_t>(0, required_free_space - d.free_space_on_field);

  if (d.treasury_white_space >= out.required_expansion_cells) {
    out.should_expand = out.required_expansion_cells > 0;
    if (!out.should_expand) {
      state_.liquidity_constraint_mode_active = false;
    }
    return out;
  }

  if (!state_.liquidity_constraint_mode_active) {
    out.entered_liquidity_constraint_mode = true;
  }
  state_.liquidity_constraint_mode_active = true;
  state_.spatial_expansion_failures_current_period += 1;
  return out;
}

PeriodCloseDecision StabilizationEngine::EvaluatePeriodClose(const std::string& period_id,
                                                             const StabilizationDerived& d,
                                                             int64_t money_supply) {
  PeriodCloseDecision out;
  if (period_id.empty()) return out;

  if (state_.last_stabilization_action_period_id == period_id) {
    out.already_handled_for_period = true;
    return out;
  }

  const bool stress_lcr = d.lcr < cfg_.lcr_stress_threshold;
  const bool stress_failures = state_.spatial_expansion_failures_current_period > 3;
  const bool should_expand = stress_lcr || stress_failures;

  if (should_expand) {
    const int64_t required_treasury_space =
        static_cast<int64_t>(std::ceil(cfg_.target_lcr * static_cast<double>(std::max<int64_t>(d.deployed_capital, 0))));
    const int64_t needed_space = std::max<int64_t>(0, required_treasury_space - d.treasury_white_space);
    out.needed_money = static_cast<int64_t>(std::ceil(static_cast<double>(needed_space) / static_cast<double>(std::max<int64_t>(d.k_lend, 1))));
    out.max_growth_money = static_cast<int64_t>(
        std::floor(cfg_.max_auto_money_growth * static_cast<double>(std::max<int64_t>(0, money_supply))));
    out.actual_money_expansion = std::max<int64_t>(0, std::min(out.needed_money, out.max_growth_money));
    out.should_expand_money = out.actual_money_expansion > 0;
    if (!out.should_expand_money) {
      out.should_emit_no_adjustment = true;
      state_.last_stabilization_action_type = "no_adjustment";
    } else {
      state_.last_stabilization_action_type = "monetary_expansion";
    }
  } else {
    out.should_emit_no_adjustment = true;
    state_.last_stabilization_action_type = "no_adjustment";
  }

  state_.last_stabilization_action_period_id = period_id;
  return out;
}

void StabilizationEngine::OnSpatialExpansionApplied() {
  state_.liquidity_constraint_mode_active = false;
  state_.last_stabilization_action_type = "spatial_expansion";
  state_.expansion_recent_checks_remaining = 2;
}

void StabilizationEngine::ResetForNewPeriod() {
  state_.spatial_expansion_failures_current_period = 0;
  state_.liquidity_constraint_mode_active = false;
  state_.expansion_recent_checks_remaining = 0;
}

std::string StabilizationEngine::UiStatus() const {
  if (state_.liquidity_constraint_mode_active) return "Liquidity Tightening";
  if (state_.expansion_recent_checks_remaining > 0) return "Expanding";
  return "Stable";
}

}  // namespace economy
