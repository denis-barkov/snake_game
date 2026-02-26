#include "chunk_manager.h"

#include <algorithm>

namespace world {

size_t ChunkIdHash::operator()(const ChunkId& id) const {
  return (static_cast<size_t>(static_cast<uint32_t>(id.cx)) << 32) ^
         static_cast<size_t>(static_cast<uint32_t>(id.cy));
}

ChunkManager::ChunkManager(int chunk_size, bool single_chunk_mode)
    : chunk_size_(std::max(8, chunk_size)), single_chunk_mode_(single_chunk_mode) {
  RecomputeChunkGrid();
}

void ChunkManager::SetConfig(int chunk_size, bool single_chunk_mode) {
  chunk_size_ = std::max(8, chunk_size);
  single_chunk_mode_ = single_chunk_mode;
  RecomputeChunkGrid();
}

void ChunkManager::SetWorldBounds(int world_w, int world_h) {
  world_w_ = std::max(1, world_w);
  world_h_ = std::max(1, world_h);
  RecomputeChunkGrid();
}

ChunkId ChunkManager::CoordToChunk(int x, int y) const {
  if (single_chunk_mode_) return {0, 0};
  const int clamped_x = ClampX(x);
  const int clamped_y = ClampY(y);
  const int cx = clamped_x / chunk_size_;
  const int cy = clamped_y / chunk_size_;
  return {
      std::max(0, std::min(num_chunks_x_ - 1, cx)),
      std::max(0, std::min(num_chunks_y_ - 1, cy)),
  };
}

std::vector<ChunkId> ChunkManager::GetChunksInRadius(const ChunkId& center, int radius) const {
  if (single_chunk_mode_) return {{0, 0}};
  const int safe_radius = std::max(0, radius);
  const int minx = std::max(0, center.cx - safe_radius);
  const int maxx = std::min(num_chunks_x_ - 1, center.cx + safe_radius);
  const int miny = std::max(0, center.cy - safe_radius);
  const int maxy = std::min(num_chunks_y_ - 1, center.cy + safe_radius);
  std::vector<ChunkId> out;
  out.reserve(static_cast<size_t>((maxx - minx + 1) * (maxy - miny + 1)));
  for (int cx = minx; cx <= maxx; ++cx) {
    for (int cy = miny; cy <= maxy; ++cy) {
      out.push_back({cx, cy});
    }
  }
  return out;
}

Vec2 ChunkManager::ChunkCenterToWorld(const ChunkId& id) const {
  if (single_chunk_mode_) return {world_w_ / 2, world_h_ / 2};
  int cx = std::max(0, std::min(num_chunks_x_ - 1, id.cx));
  int cy = std::max(0, std::min(num_chunks_y_ - 1, id.cy));
  int x = cx * chunk_size_ + chunk_size_ / 2;
  int y = cy * chunk_size_ + chunk_size_ / 2;
  return {ClampX(x), ClampY(y)};
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
  snake_body_chunks_.clear();

  for (const auto& s : snakes) {
    if (!s.alive || s.body.empty()) continue;
    const ChunkId head_id = CoordToChunk(s.body.front().x, s.body.front().y);
    snake_head_chunk_[s.id] = head_id;

    auto& body_chunks = snake_body_chunks_[s.id];
    for (const auto& seg : s.body) {
      const ChunkId id = CoordToChunk(seg.x, seg.y);
      body_chunks.insert(id);
      ChunkData& chunk = EnsureChunk(id, tick_id);
      chunk.snake_ids.insert(s.id);
    }
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
  const auto it = snake_body_chunks_.find(snake_id);
  if (it == snake_body_chunks_.end()) return false;
  for (const auto& c : it->second) {
    if (chunks.find(c) != chunks.end()) return true;
  }
  return false;
}

bool ChunkManager::FoodInChunks(const Food& food, const std::unordered_set<ChunkId, ChunkIdHash>& chunks) const {
  const ChunkId id = CoordToChunk(food.x, food.y);
  return chunks.find(id) != chunks.end();
}

int ChunkManager::ClampX(int x) const {
  return std::max(0, std::min(world_w_ - 1, x));
}

int ChunkManager::ClampY(int y) const {
  return std::max(0, std::min(world_h_ - 1, y));
}

void ChunkManager::RecomputeChunkGrid() {
  if (single_chunk_mode_) {
    num_chunks_x_ = 1;
    num_chunks_y_ = 1;
    return;
  }
  // Ceil-div per requirements: (world + chunk - 1) / chunk.
  num_chunks_x_ = std::max(1, (world_w_ + chunk_size_ - 1) / chunk_size_);
  num_chunks_y_ = std::max(1, (world_h_ + chunk_size_ - 1) / chunk_size_);
}

}  // namespace world
