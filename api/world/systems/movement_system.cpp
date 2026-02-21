#include "movement_system.h"

#include <unordered_map>

namespace world {

void MovementSystem::Run(std::vector<Snake>& snakes, std::unordered_map<int, InputIntent>& input_buffer, int width, int height) {
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

  std::unordered_map<int, Vec2> next_head;
  next_head.reserve(snakes.size());

  for (auto& s : snakes) {
    if (!s.alive || s.paused || s.dir == Dir::Stop || s.body.empty()) continue;
    next_head[s.id] = StepWrapped(s.body[0], s.dir, width, height);
  }

  for (auto& s : snakes) {
    if (!s.alive) continue;
    auto it = next_head.find(s.id);
    if (it == next_head.end()) continue;

    s.body.insert(s.body.begin(), it->second);
    if (s.grow > 0) {
      --s.grow;
    } else if (!s.body.empty()) {
      s.body.pop_back();
    }
  }
}

}  // namespace world
