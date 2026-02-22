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
  std::string role = "player";
  int64_t created_at = 0;
  std::string company_name;
};

struct Snake {
  std::string snake_id;
  std::string owner_user_id;
  bool alive = true;
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
  int64_t m_gov_reserve = 400;
  int64_t cap_delta_m = 5000;
  int64_t delta_m_issue = 0;
  int64_t delta_k_obs = 0;
  int64_t updated_at = 0;
  std::string updated_by;
};

struct EconomyPeriod {
  std::string period_key;
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

}  // namespace storage
