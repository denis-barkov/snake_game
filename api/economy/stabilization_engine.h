#pragma once

#include <cstdint>
#include <string>

#include "../world/world.h"

namespace economy {

// Runtime-configured parameters for automatic stabilization.
struct StabilizationConfig {
  bool auto_expansion_enabled = true;
  double auto_expansion_trigger_ratio = 2.0;
  double target_spatial_ratio = 3.2;
  int auto_expansion_checks_per_period = 48;
  double target_lcr = 1.2;
  double lcr_stress_threshold = 0.7;
  double max_auto_money_growth = 0.08;
};

// Authoritative derived metrics used by both fast and slow stabilization paths.
struct StabilizationDerived {
  int64_t money_supply = 0;
  int64_t k_lend = 1;
  int64_t deployed_capital = 0;
  int64_t field_size = 0;
  int64_t free_space_on_field = 0;
  int64_t total_theoretical_space = 0;
  int64_t treasury_white_space = 0;
  double spatial_ratio_r = 0.0;
  double lcr = 0.0;
};

// Runtime-only counters/state required by the stabilization proposal.
struct StabilizationRuntimeState {
  int64_t spatial_expansion_failures_current_period = 0;
  bool liquidity_constraint_mode_active = false;
  std::string last_stabilization_action_period_id;
  std::string last_stabilization_action_type;
  int expansion_recent_checks_remaining = 0;
};

struct FastCheckDecision {
  bool triggered = false;
  int64_t required_expansion_cells = 0;
  bool should_expand = false;
  bool entered_liquidity_constraint_mode = false;
};

struct PeriodCloseDecision {
  bool already_handled_for_period = false;
  bool should_expand_money = false;
  bool should_emit_no_adjustment = false;
  int64_t needed_money = 0;
  int64_t max_growth_money = 0;
  int64_t actual_money_expansion = 0;
};

class StabilizationEngine {
 public:
  explicit StabilizationEngine(StabilizationConfig cfg);

  static int64_t ComputeOccupiedSnakeCells(const world::WorldSnapshot& world);

  StabilizationDerived Derive(int64_t money_supply,
                              int64_t k_lend,
                              int64_t deployed_capital,
                              int64_t field_size,
                              int64_t free_space_on_field) const;

  FastCheckDecision EvaluateFastSpatialCheck(const StabilizationDerived& d);

  PeriodCloseDecision EvaluatePeriodClose(const std::string& period_id,
                                          const StabilizationDerived& d,
                                          int64_t money_supply);

  void OnSpatialExpansionApplied();
  void ResetForNewPeriod();
  std::string UiStatus() const;

  const StabilizationRuntimeState& runtime_state() const { return state_; }

 private:
  StabilizationConfig cfg_;
  StabilizationRuntimeState state_;
};

}  // namespace economy
