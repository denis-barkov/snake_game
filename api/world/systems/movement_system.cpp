#include "movement_system.h"

#include <unordered_map>

namespace world {

void MovementSystem::Run(std::vector<Snake>& snakes, std::unordered_map<int, InputIntent>& input_buffer, int width, int height) {
  (void)width;
  (void)height;
  // Apply network intents once per tick so the network layer never mutates world state directly.
  if (!input_buffer.empty()) {
    for (auto& s : snakes) {
      auto it = input_buffer.find(s.id);
      if (it == input_buffer.end()) continue;
      const InputIntent& intent = it->second;
      if (intent.has_desired_dir) {
        s.dir = intent.desired_dir;
        s.paused = false;
      }
      if (intent.toggle_pause) {
        s.paused = !s.paused;
      }
    }
    input_buffer.clear();
  }
}

}  // namespace world
