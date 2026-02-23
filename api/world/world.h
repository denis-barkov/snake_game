#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../storage/models.h"
#include "chunk_manager.h"
#include "entities/food.h"
#include "entities/obstacle.h"
#include "entities/snake.h"
#include "systems/collision_system.h"
#include "systems/movement_system.h"

namespace world {

struct WorldSnapshot {
  std::vector<Snake> snakes;
  std::vector<Food> foods;
  uint64_t tick = 0;
  int w = 0;
  int h = 0;
};

struct PersistenceDelta {
  std::vector<storage::Snake> upsert_snakes;
  std::vector<std::string> delete_snake_ids;
  std::optional<storage::WorldChunk> upsert_world_chunk;
  std::vector<storage::SnakeEvent> snake_events;

  bool empty() const {
    return upsert_snakes.empty() && delete_snake_ids.empty() && !upsert_world_chunk.has_value() && snake_events.empty();
  }
};

class World {
 public:
  World(int width, int height, int food_count, int max_snakes_per_user);

  // Loads in-memory world from object-based persistence tables.
  void LoadFromStorage(const std::vector<storage::Snake>& snakes,
                       const std::optional<storage::WorldChunk>& world_chunk);

  // Tick order is fixed and deterministic: movement -> collision -> spawn -> tick++.
  void Tick();

  uint64_t TickId() const;
  int Width() const;
  int Height() const;
  std::vector<Snake> Snakes() const;
  std::vector<Food> Foods() const;
  Obstacles ObstaclesList() const;
  WorldSnapshot Snapshot() const;
  WorldSnapshot SnapshotForCamera(int camera_x, int camera_y, bool aoi_enabled, int aoi_radius) const;
  void ConfigureChunking(int chunk_size, bool single_chunk_mode);

  // Network layer writes user intents; systems consume them on Tick().
  bool QueueDirectionInput(int user_id, int snake_id, Dir d);
  bool QueuePauseToggle(int user_id, int snake_id);

  std::vector<Snake> ListUserSnakes(int user_id) const;
  std::optional<int> CreateSnakeForUser(int user_id, const std::string& color);

  // Drains only meaningful state mutations (no per-tick movement writes).
  PersistenceDelta DrainPersistenceDelta(int64_t ts_ms);

 private:
  static int ToInt(const std::string& s);
  static std::string ColorForUser(int user_id);
  static std::string EncodeBody(const std::vector<Vec2>& body);
  static std::vector<Vec2> DecodeBody(const std::string& body_compact);
  static std::string EncodeFoods(const std::vector<Food>& foods);
  static std::vector<Food> DecodeFoods(const std::string& food_state);

  Snake* FindSnakeLocked(int snake_id);
  const Snake* FindSnakeLocked(int snake_id) const;
  void ResolveOverlapsOnStartLocked();
  void MarkSnakeDirtyLocked(int snake_id);
  void PushSnakeEventLocked(const CollisionEvent& e, int64_t created_at);

  mutable std::mutex mu_;
  int width_;
  int height_;
  int food_count_;
  int max_snakes_per_user_;

  uint64_t tick_ = 0;
  int64_t world_version_ = 0;
  int next_snake_id_ = 1;

  std::vector<Snake> snakes_;
  std::vector<Food> foods_;
  Obstacles obstacles_;

  std::unordered_map<int, InputIntent> input_buffer_;
  std::unordered_map<int, int64_t> snake_created_at_ms_;
  std::unordered_set<int> dirty_snake_ids_;
  std::unordered_set<int> deleted_snake_ids_;
  std::vector<storage::SnakeEvent> pending_snake_events_;
  bool world_chunk_dirty_ = false;

  std::mt19937 rng_;
  ChunkManager chunk_manager_;
};

}  // namespace world
