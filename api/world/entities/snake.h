#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace world {

struct Vec2 {
  int x = 0;
  int y = 0;

  bool operator==(const Vec2& o) const {
    return x == o.x && y == o.y;
  }
};

enum class Dir : int {
  Stop = 0,
  Left = 1,
  Right = 2,
  Up = 3,
  Down = 4,
};

Dir OppositeDir(Dir d);
Vec2 StepWrapped(Vec2 p, Dir d, int width, int height);

struct Snake {
  int id = 0;
  int user_id = 0;
  std::string color = "#00ff00";
  Dir dir = Dir::Stop;
  bool paused = false;
  bool alive = true;
  int grow = 0;
  std::vector<Vec2> body;
  // Guards to prevent repeated punishment/drain within the same simulation tick.
  uint64_t last_reverse_tick = UINT64_MAX;
  uint64_t last_loss_tick = UINT64_MAX;
  // Side-head duel state (resolved asynchronously after ~1 second in ticks).
  bool duel_pending = false;
  int duel_with_id = 0;
  uint64_t duel_resolve_tick = 0;
};

}  // namespace world
