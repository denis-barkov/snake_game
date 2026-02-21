#pragma once

#include <random>
#include <utility>
#include <vector>

#include "../entities/food.h"
#include "../entities/snake.h"

namespace world {

class CollisionSystem {
 public:
  // Resolves collisions using the current gameplay rules (self, snake-vs-snake, snake-vs-food).
  static void Run(std::vector<Snake>& snakes, std::vector<Food>& foods, std::vector<std::pair<int, int>>& tombstones, int width, int height, std::mt19937& rng);
};

}  // namespace world
