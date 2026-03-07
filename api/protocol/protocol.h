#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "protocol_version.h"

namespace protocol {

enum class MsgType : uint8_t {
  Snapshot = 1,
};

struct Vec2 {
  int x = 0;
  int y = 0;
};

struct SnakeState {
  int id = 0;
  int user_id = 0;
  std::string color;
  int dir = 0;
  bool paused = false;
  std::vector<Vec2> body;
};

struct Snapshot {
  uint64_t tick = 0;
  int w = 0;
  int h = 0;
  std::vector<Vec2> foods;
  std::vector<SnakeState> snakes;
};

}  // namespace protocol
