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
  int64_t created_at = 0;
};

struct SnakeCheckpoint {
  std::string snake_id;
  std::string owner_user_id;
  int64_t ts = 0;

  int dir = 0;
  bool paused = false;
  std::vector<std::pair<int, int>> body;

  int length = 0;
  int score = 0;
  int w = 0;
  int h = 0;
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
  int64_t computed_at = 0;
};

struct Event {
  std::string pk;
  std::string sk;
  std::string type;
  std::string payload_json;
  int64_t created_at = 0;
};

}  // namespace storage
