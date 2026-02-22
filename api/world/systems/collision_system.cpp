#include "collision_system.h"

#include <algorithm>
#include <unordered_map>

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

void CollisionSystem::Run(std::vector<Snake>& snakes,
                          std::vector<Food>& foods,
                          int width,
                          int height,
                          std::mt19937& rng,
                          std::vector<CollisionEvent>& events,
                          bool& food_changed) {
  (void)width;
  (void)height;
  food_changed = false;

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
      events.push_back(CollisionEvent{"SELF_COLLISION", s.id, 0, h.x, h.y, -1});
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

    const Vec2 impact = attacker->body[0];
    attacker->grow += 1;
    attacker->dir = OppositeDir(attacker->dir);
    attacker->paused = false;
    events.push_back(CollisionEvent{"BITE", attacker->id, defender->id, impact.x, impact.y, 1});

    if (!defender->body.empty()) {
      defender->body.pop_back();
      events.push_back(CollisionEvent{"BITTEN", defender->id, attacker->id, impact.x, impact.y, -1});
    }
    if (defender->body.empty()) defender->alive = false;
  }

  for (auto& s : snakes) {
    if (!s.alive || s.body.empty()) continue;
    const Vec2 head = s.body[0];
    for (auto& f : foods) {
      if (f.x == head.x && f.y == head.y) {
        s.grow += 1;
        events.push_back(CollisionEvent{"FOOD", s.id, 0, head.x, head.y, 1});
        Vec2 replacement = SpawnSystem::RandFreeCell(snakes, foods, width, height, rng);
        f.x = replacement.x;
        f.y = replacement.y;
        food_changed = true;
      }
    }
  }

  for (const auto& s : snakes) {
    if (!s.alive) {
      int x = 0;
      int y = 0;
      if (!s.body.empty()) {
        x = s.body[0].x;
        y = s.body[0].y;
      }
      events.push_back(CollisionEvent{"DEATH", s.id, 0, x, y, -1});
    }
  }

  snakes.erase(std::remove_if(snakes.begin(), snakes.end(), [](const Snake& s) { return !s.alive; }), snakes.end());
}

}  // namespace world
