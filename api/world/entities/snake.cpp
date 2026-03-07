#include "snake.h"

namespace world {

Dir OppositeDir(Dir d) {
  switch (d) {
    case Dir::Left:
      return Dir::Right;
    case Dir::Right:
      return Dir::Left;
    case Dir::Up:
      return Dir::Down;
    case Dir::Down:
      return Dir::Up;
    default:
      return Dir::Stop;
  }
}

Vec2 StepWrapped(Vec2 p, Dir d, int width, int height) {
  switch (d) {
    case Dir::Left:
      --p.x;
      break;
    case Dir::Right:
      ++p.x;
      break;
    case Dir::Up:
      --p.y;
      break;
    case Dir::Down:
      ++p.y;
      break;
    default:
      break;
  }

  if (p.x < 0) p.x = width - 1;
  if (p.x >= width) p.x = 0;
  if (p.y < 0) p.y = height - 1;
  if (p.y >= height) p.y = 0;
  return p;
}

}  // namespace world
