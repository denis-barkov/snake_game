#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../storage/models.h"
#include "entities/food.h"
#include "entities/obstacle.h"
#include "entities/snake.h"
#include "systems/movement_system.h"

namespace world {

struct WorldSnapshot {
  std::vector<Snake> snakes;
  std::vector<Food> foods;
  uint64_t tick = 0;
  int w = 0;
  int h = 0;
};

class World {
 public:
  struct PersistenceBatch {
    std::vector<storage::SnakeCheckpoint> live;
    std::vector<storage::SnakeCheckpoint> tombstones;
  };

  World(int width, int height, int food_count, int max_snakes_per_user);

  // Loads world state from storage records and keeps runtime behavior identical.
  void LoadFromCheckpoints(const std::vector<storage::SnakeCheckpoint>& checkpoints);

  // Tick order is fixed and deterministic: movement -> collision -> spawn -> tick++.
  void Tick();

  uint64_t TickId() const;
  int Width() const;
  int Height() const;
  std::vector<Snake> Snakes() const;
  std::vector<Food> Foods() const;
  Obstacles ObstaclesList() const;
  WorldSnapshot Snapshot() const;

  // Network layer writes user intents; systems consume them on Tick().
  bool QueueDirectionInput(int user_id, int snake_id, Dir d);
  bool QueuePauseToggle(int user_id, int snake_id);

  std::vector<Snake> ListUserSnakes(int user_id) const;
  std::optional<int> CreateSnakeForUser(int user_id, const std::string& color);

  PersistenceBatch BuildPersistenceBatch(int64_t ts_ms);

 private:
  static int ToInt(const std::string& s);
  static std::string ColorForUser(int user_id);

  Snake* FindSnakeLocked(int snake_id);
  void ResolveOverlapsOnStartLocked();

  mutable std::mutex mu_;
  int width_;
  int height_;
  int food_count_;
  int max_snakes_per_user_;

  uint64_t tick_ = 0;
  int next_snake_id_ = 1;

  std::vector<Snake> snakes_;
  std::vector<Food> foods_;
  Obstacles obstacles_;

  std::unordered_map<int, InputIntent> input_buffer_;
  std::vector<std::pair<int, int>> pending_tombstones_;

  std::mt19937 rng_;
};

}  // namespace world
