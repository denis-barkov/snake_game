#pragma once

#include <unordered_map>
#include <vector>

#include "../entities/snake.h"

namespace world {

struct InputIntent {
  bool has_desired_dir = false;
  Dir desired_dir = Dir::Stop;
  bool toggle_pause = false;
};

class MovementSystem {
 public:
  // Applies queued player intents and advances snake bodies one simulation step.
  static void Run(std::vector<Snake>& snakes, std::unordered_map<int, InputIntent>& input_buffer, int width, int height);
};

}  // namespace world
