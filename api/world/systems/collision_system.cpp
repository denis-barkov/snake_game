#include "collision_system.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

#include "spawn_system.h"

namespace world {

namespace {

long long CellKey(const Vec2& v) {
  return (static_cast<long long>(v.x) << 32) ^ static_cast<unsigned long long>(v.y & 0xffffffff);
}

Snake* FindSnakeById(std::vector<Snake>& snakes, int snake_id) {
  for (auto& s : snakes) {
    if (s.id == snake_id) return &s;
  }
  return nullptr;
}

}  // namespace

void CollisionSystem::Run(std::vector<Snake>& snakes, std::vector<Food>& foods, std::vector<std::pair<int, int>>& tombstones, int width, int height, std::mt19937& rng) {
  for (auto& s : snakes) {
    if (!s.alive || s.body.size() < 2) continue;
    const Vec2 h = s.body[0];
    bool hit_self = false;
    for (size_t i = 1; i < s.body.size(); ++i) {
      if (s.body[i] == h) {
        hit_self = true;
        break;
      }
    }
    if (hit_self) {
      if (!s.body.empty()) s.body.pop_back();
      s.paused = true;
      if (s.body.empty()) s.alive = false;
    }
  }

  std::unordered_map<long long, std::vector<int>> cell_owners;
  for (auto& s : snakes) {
    if (!s.alive) continue;
    for (auto& c : s.body) {
      cell_owners[CellKey(c)].push_back(s.id);
    }
  }

  std::vector<int> snake_ids;
  snake_ids.reserve(snakes.size());
  for (const auto& s : snakes) {
    if (s.alive) snake_ids.push_back(s.id);
  }
  std::sort(snake_ids.begin(), snake_ids.end());

  for (int sid : snake_ids) {
    Snake* attacker = FindSnakeById(snakes, sid);
    if (!attacker || !attacker->alive || attacker->body.empty()) continue;

    auto it = cell_owners.find(CellKey(attacker->body[0]));
    if (it == cell_owners.end()) continue;

    int defender_id = 0;
    for (int owner_id : it->second) {
      if (owner_id != attacker->id) {
        defender_id = owner_id;
        break;
      }
    }
    if (defender_id == 0) continue;

    Snake* defender = FindSnakeById(snakes, defender_id);
    if (!defender || !defender->alive) continue;

    attacker->grow += 1;
    attacker->dir = OppositeDir(attacker->dir);
    attacker->paused = false;

    if (!defender->body.empty()) defender->body.pop_back();
    if (defender->body.empty()) defender->alive = false;
  }

  for (const auto& s : snakes) {
    if (!s.alive) tombstones.push_back({s.id, s.user_id});
  }
  snakes.erase(std::remove_if(snakes.begin(), snakes.end(), [](const Snake& s) { return !s.alive; }), snakes.end());

  for (auto& s : snakes) {
    if (!s.alive || s.body.empty()) continue;
    const Vec2 head = s.body[0];
    for (auto& f : foods) {
      if (f.x == head.x && f.y == head.y) {
        s.grow += 1;
        Vec2 replacement = SpawnSystem::RandFreeCell(snakes, foods, width, height, rng);
        f.x = replacement.x;
        f.y = replacement.y;
      }
    }
  }
}

}  // namespace world
