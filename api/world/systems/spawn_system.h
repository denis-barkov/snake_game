#pragma once

#include <random>
#include <vector>

#include "../entities/food.h"
#include "../entities/snake.h"

namespace world {

class SpawnSystem {
 public:
  // Ensures food count and placement follow current spawn behavior.
  static void Run(std::vector<Snake>& snakes, std::vector<Food>& foods, int food_count, int width, int height, std::mt19937& rng);

  // Shared helper used by both spawn and collision systems.
  static Vec2 RandFreeCell(const std::vector<Snake>& snakes, const std::vector<Food>& foods, int width, int height, std::mt19937& rng);
};

}  // namespace world
