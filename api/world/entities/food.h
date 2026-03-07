#pragma once

namespace world {

struct Food {
  int x = 0;
  int y = 0;

  bool operator==(const Food& o) const {
    return x == o.x && y == o.y;
  }
};

}  // namespace world
