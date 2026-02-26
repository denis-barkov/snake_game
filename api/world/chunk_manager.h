#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "entities/food.h"
#include "entities/obstacle.h"
#include "entities/snake.h"

namespace world {

struct ChunkId {
  int cx = 0;
  int cy = 0;

  bool operator==(const ChunkId& other) const {
    return cx == other.cx && cy == other.cy;
  }
};

struct ChunkIdHash {
  size_t operator()(const ChunkId& id) const;
};

struct ChunkData {
  ChunkId id;
  std::unordered_set<int> snake_ids;
  std::vector<Food> foods;
  std::vector<Vec2> obstacles;
  bool dirty = false;
  uint64_t dirty_since_tick = 0;
};

class ChunkManager {
 public:
  ChunkManager(int chunk_size = 64, bool single_chunk_mode = true);

  void SetConfig(int chunk_size, bool single_chunk_mode);
  void SetWorldBounds(int world_w, int world_h);
  ChunkId CoordToChunk(int x, int y) const;
  std::vector<ChunkId> GetChunksInRadius(const ChunkId& center, int radius) const;
  Vec2 ChunkCenterToWorld(const ChunkId& id) const;

  void Rebuild(const std::vector<Snake>& snakes,
               const std::vector<Food>& foods,
               const Obstacles& obstacles,
               uint64_t tick_id);

  const std::unordered_map<ChunkId, ChunkData, ChunkIdHash>& Chunks() const;
  bool SnakeInChunks(int snake_id, const std::unordered_set<ChunkId, ChunkIdHash>& chunks) const;
  bool FoodInChunks(const Food& food, const std::unordered_set<ChunkId, ChunkIdHash>& chunks) const;

 private:
  ChunkData& EnsureChunk(const ChunkId& id, uint64_t tick_id);
  int ClampX(int x) const;
  int ClampY(int y) const;
  void RecomputeChunkGrid();

  int chunk_size_ = 64;
  bool single_chunk_mode_ = true;
  int world_w_ = 40;
  int world_h_ = 20;
  int num_chunks_x_ = 1;
  int num_chunks_y_ = 1;
  std::unordered_map<ChunkId, ChunkData, ChunkIdHash> chunks_;
  std::unordered_map<int, ChunkId> snake_head_chunk_;
  std::unordered_map<int, std::unordered_set<ChunkId, ChunkIdHash>> snake_body_chunks_;
};

}  // namespace world
