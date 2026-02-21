#include "world.h"

#include <algorithm>
#include <unordered_set>

#include "systems/collision_system.h"
#include "systems/spawn_system.h"

namespace world {

World::World(int width, int height, int food_count, int max_snakes_per_user)
    : width_(width),
      height_(height),
      food_count_(food_count),
      max_snakes_per_user_(max_snakes_per_user),
      rng_(static_cast<uint32_t>(std::random_device{}())) {}

void World::LoadFromCheckpoints(const std::vector<storage::SnakeCheckpoint>& checkpoints) {
  std::lock_guard<std::mutex> lock(mu_);

  snakes_.clear();
  foods_.clear();
  input_buffer_.clear();
  pending_tombstones_.clear();

  int max_snake_id = 0;
  for (const auto& cp : checkpoints) {
    if (cp.body.empty() || cp.length <= 0) continue;

    Snake s;
    s.id = ToInt(cp.snake_id);
    s.user_id = ToInt(cp.owner_user_id);
    s.dir = static_cast<Dir>(cp.dir);
    s.paused = cp.paused;
    s.alive = true;
    s.grow = 0;
    s.color = ColorForUser(s.user_id);
    s.body.reserve(cp.body.size());

    for (const auto& cell : cp.body) {
      s.body.push_back({cell.first, cell.second});
    }

    if (!s.body.empty() && s.id > 0 && s.user_id > 0) {
      snakes_.push_back(std::move(s));
      max_snake_id = std::max(max_snake_id, snakes_.back().id);
    }
  }

  next_snake_id_ = max_snake_id + 1;
  SpawnSystem::Run(snakes_, foods_, food_count_, width_, height_, rng_);
  ResolveOverlapsOnStartLocked();
}

void World::Tick() {
  std::lock_guard<std::mutex> lock(mu_);
  MovementSystem::Run(snakes_, input_buffer_, width_, height_);
  CollisionSystem::Run(snakes_, foods_, pending_tombstones_, width_, height_, rng_);
  SpawnSystem::Run(snakes_, foods_, food_count_, width_, height_, rng_);
  ++tick_;
}

uint64_t World::TickId() const {
  std::lock_guard<std::mutex> lock(mu_);
  return tick_;
}

int World::Width() const {
  std::lock_guard<std::mutex> lock(mu_);
  return width_;
}

int World::Height() const {
  std::lock_guard<std::mutex> lock(mu_);
  return height_;
}

std::vector<Snake> World::Snakes() const {
  std::lock_guard<std::mutex> lock(mu_);
  return snakes_;
}

std::vector<Food> World::Foods() const {
  std::lock_guard<std::mutex> lock(mu_);
  return foods_;
}

Obstacles World::ObstaclesList() const {
  std::lock_guard<std::mutex> lock(mu_);
  return obstacles_;
}

WorldSnapshot World::Snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  WorldSnapshot snap;
  snap.tick = tick_;
  snap.w = width_;
  snap.h = height_;
  snap.snakes = snakes_;
  snap.foods = foods_;
  return snap;
}

bool World::QueueDirectionInput(int user_id, int snake_id, Dir d) {
  std::lock_guard<std::mutex> lock(mu_);
  Snake* s = FindSnakeLocked(snake_id);
  if (!s || s->user_id != user_id) return false;

  InputIntent& intent = input_buffer_[snake_id];
  intent.has_desired_dir = true;
  intent.desired_dir = d;
  return true;
}

bool World::QueuePauseToggle(int user_id, int snake_id) {
  std::lock_guard<std::mutex> lock(mu_);
  Snake* s = FindSnakeLocked(snake_id);
  if (!s || s->user_id != user_id) return false;

  InputIntent& intent = input_buffer_[snake_id];
  // Preserve intent parity when multiple pause commands arrive before the next tick.
  intent.toggle_pause = !intent.toggle_pause;
  return true;
}

std::vector<Snake> World::ListUserSnakes(int user_id) const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<Snake> out;
  for (const auto& s : snakes_) {
    if (s.user_id == user_id) out.push_back(s);
  }
  return out;
}

std::optional<int> World::CreateSnakeForUser(int user_id, const std::string& color) {
  std::lock_guard<std::mutex> lock(mu_);

  int count = 0;
  for (const auto& s : snakes_) {
    if (s.user_id == user_id) ++count;
  }
  if (count >= max_snakes_per_user_) return std::nullopt;

  Snake s;
  s.id = next_snake_id_++;
  s.user_id = user_id;
  s.color = color;
  s.dir = Dir::Stop;
  s.paused = false;
  s.alive = true;
  s.grow = 0;

  const Vec2 p = SpawnSystem::RandFreeCell(snakes_, foods_, width_, height_, rng_);
  s.body = {p};

  snakes_.push_back(s);
  return s.id;
}

World::PersistenceBatch World::BuildPersistenceBatch(int64_t ts_ms) {
  std::lock_guard<std::mutex> lock(mu_);

  PersistenceBatch batch;
  batch.live.reserve(snakes_.size());
  for (const auto& s : snakes_) {
    storage::SnakeCheckpoint cp;
    cp.snake_id = std::to_string(s.id);
    cp.owner_user_id = std::to_string(s.user_id);
    cp.ts = ts_ms;
    cp.dir = static_cast<int>(s.dir);
    cp.paused = s.paused;
    cp.length = static_cast<int>(s.body.size());
    cp.score = static_cast<int>(s.body.size());
    cp.w = width_;
    cp.h = height_;
    cp.body.reserve(s.body.size());
    for (const auto& cell : s.body) {
      cp.body.push_back({cell.x, cell.y});
    }
    batch.live.push_back(std::move(cp));
  }

  batch.tombstones.reserve(pending_tombstones_.size());
  for (const auto& t : pending_tombstones_) {
    storage::SnakeCheckpoint cp;
    cp.snake_id = std::to_string(t.first);
    cp.owner_user_id = std::to_string(t.second);
    cp.ts = ts_ms;
    cp.dir = static_cast<int>(Dir::Stop);
    cp.paused = true;
    cp.length = 0;
    cp.score = 0;
    cp.w = width_;
    cp.h = height_;
    batch.tombstones.push_back(std::move(cp));
  }
  pending_tombstones_.clear();

  return batch;
}

int World::ToInt(const std::string& s) {
  try {
    return std::stoi(s);
  } catch (...) {
    return 0;
  }
}

std::string World::ColorForUser(int user_id) {
  static const std::vector<std::string> palette = {
      "#00ff00", "#00aaff", "#ff00ff", "#ff8800", "#00ffaa", "#ffaa00"};
  if (user_id <= 0) return "#00ff00";
  return palette[static_cast<size_t>(user_id - 1) % palette.size()];
}

Snake* World::FindSnakeLocked(int snake_id) {
  for (auto& s : snakes_) {
    if (s.id == snake_id) return &s;
  }
  return nullptr;
}

void World::ResolveOverlapsOnStartLocked() {
  auto key = [](const Vec2& v) -> long long {
    return (static_cast<long long>(v.x) << 32) ^ static_cast<unsigned long long>(v.y & 0xffffffff);
  };

  std::unordered_set<long long> occupied;

  for (auto& s : snakes_) {
    if (!s.alive) continue;
    if (s.body.empty()) {
      s.body.push_back(SpawnSystem::RandFreeCell(snakes_, foods_, width_, height_, rng_));
    }

    bool overlaps = false;
    for (const auto& c : s.body) {
      if (occupied.count(key(c))) {
        overlaps = true;
        break;
      }
    }

    if (overlaps) {
      s.body = {SpawnSystem::RandFreeCell(snakes_, foods_, width_, height_, rng_)};
      s.grow = 0;
      s.dir = Dir::Stop;
      s.paused = false;
    }

    for (const auto& c : s.body) {
      occupied.insert(key(c));
    }
  }
}

}  // namespace world
