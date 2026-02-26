#include "replication_system.h"

#include <atomic>
#include <iostream>
#include <unordered_set>

namespace world {

namespace {

bool InBounds(const Vec2& v, int w, int h) {
  return v.x >= 0 && v.x < w && v.y >= 0 && v.y < h;
}

}  // namespace

WorldSnapshot ReplicationSystem::BuildSnapshot(const WorldSnapshot& source,
                                               const ChunkManager& chunk_manager,
                                               const ReplicationRequest& req) {
  WorldSnapshot out = source;
  static std::atomic<bool> logged_invalid_once{false};

  auto sanitize = [&](const WorldSnapshot& in) {
    WorldSnapshot s = in;
    s.snakes.clear();
    s.foods.clear();
    s.snakes.reserve(in.snakes.size());
    s.foods.reserve(in.foods.size());
    bool saw_invalid = false;

    for (const auto& snake : in.snakes) {
      Snake copy = snake;
      copy.body.clear();
      copy.body.reserve(snake.body.size());
      for (const auto& seg : snake.body) {
        if (InBounds(seg, in.w, in.h)) {
          copy.body.push_back(seg);
        } else {
          saw_invalid = true;
        }
      }
      if (!copy.body.empty()) s.snakes.push_back(std::move(copy));
    }

    for (const auto& f : in.foods) {
      Vec2 p{f.x, f.y};
      if (InBounds(p, in.w, in.h)) {
        s.foods.push_back(f);
      } else {
        saw_invalid = true;
      }
    }

    if (req.debug_validate_bounds && saw_invalid && !logged_invalid_once.exchange(true)) {
      std::cerr << "[replication] dropped out-of-bounds cells in snapshot "
                << "(world_w=" << in.w << ", world_h=" << in.h << ")\n";
    }
    return s;
  };

  if (!req.aoi_enabled) return sanitize(out);

  const ChunkId center = chunk_manager.CoordToChunk(req.camera_x, req.camera_y);
  const int effective_radius = std::max(0, req.aoi_radius + req.aoi_pad_chunks);
  const auto visible = chunk_manager.GetChunksInRadius(center, effective_radius);
  std::unordered_set<ChunkId, ChunkIdHash> visible_set(visible.begin(), visible.end());

  out.snakes.clear();
  out.foods.clear();
  out.snakes.reserve(source.snakes.size());
  out.foods.reserve(source.foods.size());

  for (const auto& s : source.snakes) {
    if (chunk_manager.SnakeInChunks(s.id, visible_set)) {
      out.snakes.push_back(s);
    }
  }

  for (const auto& f : source.foods) {
    if (chunk_manager.FoodInChunks(f, visible_set)) {
      out.foods.push_back(f);
    }
  }

  return sanitize(out);
}

}  // namespace world
