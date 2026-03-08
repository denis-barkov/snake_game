#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace storage {

struct User {
  std::string user_id;
  std::string username;
  std::string password_hash;
  int64_t balance_mi = 0;
  // V2/V3 placeholder fields (not used by V1 compute yet).
  int64_t debt_principal = 0;
  double debt_interest_rate = 0.0;
  int64_t debt_accrued_interest = 0;
  std::string role = "player";
  int64_t created_at = 0;
  int64_t updated_at = 0;
  std::string company_name;
  std::string company_name_normalized;
  std::string last_seen_world_version;
  std::string auth_provider = "local";
  std::string google_subject_id;
  bool onboarding_completed = false;
  std::string starter_snake_id;
  std::string account_status = "active";
};

struct Snake {
  std::string snake_id;
  std::string owner_user_id;
  std::string snake_name;
  std::string snake_name_normalized;
  bool alive = true;
  // Explicit "on field" flag for economy aggregation (defaults to alive if missing in DB).
  bool is_on_field = true;
  int head_x = 0;
  int head_y = 0;
  int direction = 0;
  bool paused = false;
  int length_k = 0;
  std::string body_compact;
  std::string color = "#00ff00";
  std::string last_event_id;
  int64_t created_at = 0;
  int64_t updated_at = 0;
};

struct WorldChunk {
  std::string chunk_id;
  int width = 0;
  int height = 0;
  std::string obstacles;
  std::string food_state;
  int64_t version = 0;
  int64_t updated_at = 0;
};

struct SnakeEvent {
  std::string snake_id;
  std::string event_id;
  std::string event_type;  // BITE, BITTEN, FOOD, DEATH, SELF_COLLISION, SPAWN
  int x = 0;
  int y = 0;
  std::string other_snake_id;
  int delta_length = 0;
  uint64_t tick_number = 0;
  int64_t world_version = 0;
  int64_t created_at = 0;
};

struct Settings {
  std::string settings_id = "global";
  int tick_hz = 10;
  int spectator_hz = 10;
  int max_snakes_per_user = 3;
  std::string feature_flags_json = "{}";
  std::string economy_refs_json = "{}";
  int64_t updated_at = 0;
};

struct EconomyParams {
  int version = 1;
  int k_land = 24;
  double a_productivity = 1.0;
  double v_velocity = 2.0;
  int food_spawn_target = 1;
  double alpha_bootstrap_default = 0.5;
  int64_t m_gov_reserve = 400;
  int64_t cap_delta_m = 5000;
  int64_t delta_m_issue = 0;
  int64_t delta_k_obs = 0;
  int64_t updated_at = 0;
  std::string updated_by;
};

struct EconomyPeriod {
  std::string period_key;
  int64_t harvested_food = 0;
  // Abstract production counter (V1 maps this to harvested_food).
  int64_t real_output = 0;
  int64_t movement_ticks = 0;
  int64_t total_output = 0;
  int64_t total_capital = 0;
  int64_t total_labor = 0;
  double capital_share = 0.5;
  double productivity_index = 0.0;
  int64_t money_supply = 0;
  double price_index = 0.0;
  double inflation_rate = 0.0;
  bool price_index_valid = false;
  bool inflation_valid = false;
  int64_t treasury_balance = 0;
  bool alpha_bootstrap = false;
  bool is_finalized = false;
  int64_t finalized_at = 0;
  std::string snapshot_status = "live_unfinalized";
  int64_t period_ends_in_seconds = 0;
  // Legacy fields kept for backward compatibility.
  int64_t delta_m_buy = 0;
  int64_t computed_m = 0;
  int64_t computed_k = 0;
  int64_t computed_y = 0;
  int64_t computed_p = 0;
  int64_t computed_pi = 0;
  int64_t computed_world_area = 0;
  int64_t computed_white = 0;
  int64_t computed_at = 0;
};

struct EconomyPeriodUser {
  std::string period_key;
  std::string user_id;
  int64_t user_harvested_food = 0;
  // Abstract production counter (V1 maps this to user_harvested_food).
  int64_t user_real_output = 0;
  int64_t user_movement_ticks = 0;
  int64_t user_output = 0;
  int64_t user_capital = 0;
  int64_t user_labor = 0;
  double user_capital_share = 0.5;
  double user_productivity = 0.0;
  double user_market_share = 0.0;
  int64_t user_storage_balance = 0;
  bool alpha_bootstrap = false;
  int64_t computed_at = 0;
};

}  // namespace storage
