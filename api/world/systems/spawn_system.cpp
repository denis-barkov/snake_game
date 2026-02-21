#include "spawn_system.h"

#include <unordered_set>

namespace world {

namespace {

long long CellKey(const Vec2& v) {
  return (static_cast<long long>(v.x) << 32) ^ static_cast<unsigned long long>(v.y & 0xffffffff);
}

}  // namespace

Vec2 SpawnSystem::RandFreeCell(const std::vector<Snake>& snakes, const std::vector<Food>& foods, int width, int height, std::mt19937& rng) {
  std::uniform_int_distribution<int> dx(0, width - 1);
  std::uniform_int_distribution<int> dy(0, height - 1);

  std::unordered_set<long long> occupied;
  for (const auto& s : snakes) {
    if (!s.alive) continue;
    for (const auto& c : s.body) {
      occupied.insert(CellKey(c));
    }
  }
  for (const auto& f : foods) {
    occupied.insert(CellKey(Vec2{f.x, f.y}));
  }

  for (int tries = 0; tries < 2000; ++tries) {
    Vec2 candidate{dx(rng), dy(rng)};
    if (!occupied.count(CellKey(candidate))) return candidate;
  }
  return {0, 0};
}

void SpawnSystem::Run(std::vector<Snake>& snakes, std::vector<Food>& foods, int food_count, int width, int height, std::mt19937& rng) {
  while (static_cast<int>(foods.size()) < food_count) {
    Vec2 pos = RandFreeCell(snakes, foods, width, height, rng);
    foods.push_back(Food{pos.x, pos.y});
  }
}

}  // namespace world
