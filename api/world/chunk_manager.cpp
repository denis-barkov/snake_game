#include "chunk_manager.h"

#include <algorithm>
#include <cmath>

namespace world {

size_t ChunkIdHash::operator()(const ChunkId& id) const {
  return (static_cast<size_t>(static_cast<uint32_t>(id.cx)) << 32) ^
         static_cast<size_t>(static_cast<uint32_t>(id.cy));
}

ChunkManager::ChunkManager(int chunk_size, bool single_chunk_mode)
    : chunk_size_(std::max(8, chunk_size)), single_chunk_mode_(single_chunk_mode) {}

void ChunkManager::SetConfig(int chunk_size, bool single_chunk_mode) {
  chunk_size_ = std::max(8, chunk_size);
  single_chunk_mode_ = single_chunk_mode;
}

ChunkId ChunkManager::CoordToChunk(int x, int y) const {
  if (single_chunk_mode_) return {0, 0};
  const double fx = static_cast<double>(x) / static_cast<double>(chunk_size_);
  const double fy = static_cast<double>(y) / static_cast<double>(chunk_size_);
  return {static_cast<int>(std::floor(fx)), static_cast<int>(std::floor(fy))};
}

std::vector<ChunkId> ChunkManager::GetChunksInRadius(const ChunkId& center, int radius) const {
  const int safe_radius = std::max(0, radius);
  std::vector<ChunkId> out;
  out.reserve(static_cast<size_t>((safe_radius * 2 + 1) * (safe_radius * 2 + 1)));
  for (int dx = -safe_radius; dx <= safe_radius; ++dx) {
    for (int dy = -safe_radius; dy <= safe_radius; ++dy) {
      out.push_back({center.cx + dx, center.cy + dy});
    }
  }
  return out;
}

ChunkData& ChunkManager::EnsureChunk(const ChunkId& id, uint64_t tick_id) {
  auto [it, inserted] = chunks_.emplace(id, ChunkData{});
  if (inserted) {
    it->second.id = id;
    it->second.dirty = true;
    it->second.dirty_since_tick = tick_id;
  }
  return it->second;
}

void ChunkManager::Rebuild(const std::vector<Snake>& snakes,
                           const std::vector<Food>& foods,
                           const Obstacles& obstacles,
                           uint64_t tick_id) {
  chunks_.clear();
  snake_head_chunk_.clear();

  for (const auto& s : snakes) {
    if (!s.alive || s.body.empty()) continue;
    const ChunkId id = CoordToChunk(s.body.front().x, s.body.front().y);
    ChunkData& chunk = EnsureChunk(id, tick_id);
    chunk.snake_ids.insert(s.id);
    snake_head_chunk_[s.id] = id;
  }

  for (const auto& f : foods) {
    const ChunkId id = CoordToChunk(f.x, f.y);
    ChunkData& chunk = EnsureChunk(id, tick_id);
    chunk.foods.push_back(f);
  }

  for (const auto& o : obstacles) {
    const ChunkId id = CoordToChunk(o.pos.x, o.pos.y);
    ChunkData& chunk = EnsureChunk(id, tick_id);
    chunk.obstacles.push_back(o.pos);
  }
}

const std::unordered_map<ChunkId, ChunkData, ChunkIdHash>& ChunkManager::Chunks() const {
  return chunks_;
}

bool ChunkManager::SnakeInChunks(int snake_id, const std::unordered_set<ChunkId, ChunkIdHash>& chunks) const {
  const auto it = snake_head_chunk_.find(snake_id);
  if (it == snake_head_chunk_.end()) return false;
  return chunks.find(it->second) != chunks.end();
}

bool ChunkManager::FoodInChunks(const Food& food, const std::unordered_set<ChunkId, ChunkIdHash>& chunks) const {
  const ChunkId id = CoordToChunk(food.x, food.y);
  return chunks.find(id) != chunks.end();
}

}  // namespace world
