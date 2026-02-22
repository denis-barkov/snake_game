#pragma once

#include <random>
#include <string>
#include <vector>

#include "../entities/food.h"
#include "../entities/snake.h"

namespace world {

struct CollisionEvent {
  std::string event_type;  // BITE, BITTEN, FOOD, DEATH, SELF_COLLISION
  int snake_id = 0;
  int other_snake_id = 0;
  int x = 0;
  int y = 0;
  int delta_length = 0;
};

class CollisionSystem {
 public:
  // Resolves collisions using the current gameplay rules and emits meaningful gameplay events.
  static void Run(std::vector<Snake>& snakes,
                  std::vector<Food>& foods,
                  int width,
                  int height,
                  std::mt19937& rng,
                  std::vector<CollisionEvent>& events,
                  bool& food_changed);
};

}  // namespace world
