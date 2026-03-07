#pragma once

#include <functional>
#include <random>
#include <string>
#include <vector>

#include "../entities/food.h"
#include "../entities/snake.h"

namespace world {

struct CollisionEvent {
  std::string event_type;  // FOOD_EATEN, TAIL_BITE, TAIL_BITTEN, HEAD_DUEL_WIN, HEAD_DUEL_LOSS, HEAD_ONCOMING, DEATH, SELF_COLLISION
  int snake_id = 0;
  int other_snake_id = 0;
  int x = 0;
  int y = 0;
  int delta_length = 0;
  int credit_user_id = 0;
  int delta_user_cells = 0;
  int delta_system_cells = 0;
};

class CollisionSystem {
 public:
  // Resolves collisions using the current gameplay rules and emits meaningful gameplay events.
  static void Run(std::vector<Snake>& snakes,
                  std::vector<Food>& foods,
                  int width,
                  int height,
                  uint64_t tick_id,
                  int duel_delay_ticks,
                  std::mt19937& rng,
                  std::vector<CollisionEvent>& events,
                  bool& food_changed,
                  const std::function<bool(const Vec2&)>& is_playable = nullptr);
};

}  // namespace world
