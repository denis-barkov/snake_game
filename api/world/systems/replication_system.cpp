#include "replication_system.h"

#include <unordered_set>

namespace world {

WorldSnapshot ReplicationSystem::BuildSnapshot(const WorldSnapshot& source,
                                               const ChunkManager& chunk_manager,
                                               const ReplicationRequest& req) {
  WorldSnapshot out = source;
  if (!req.aoi_enabled) return out;

  const ChunkId center = chunk_manager.CoordToChunk(req.camera_x, req.camera_y);
  const auto visible = chunk_manager.GetChunksInRadius(center, req.aoi_radius);
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

  return out;
}

}  // namespace world
