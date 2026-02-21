#pragma once

#include <vector>

#include "snake.h"

namespace world {

struct Obstacle {
  Vec2 pos;
};

using Obstacles = std::vector<Obstacle>;

}  // namespace world
