#include "world.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_set>

#include "systems/spawn_system.h"

namespace world {

World::World(int width, int height, int food_count, int max_snakes_per_user)
    : width_(width),
      height_(height),
      food_count_(food_count),
      max_snakes_per_user_(max_snakes_per_user),
      rng_(static_cast<uint32_t>(std::random_device{}())) {}

void World::LoadFromStorage(const std::vector<storage::Snake>& stored_snakes,
                            const std::optional<storage::WorldChunk>& world_chunk) {
  std::lock_guard<std::mutex> lock(mu_);

  snakes_.clear();
  foods_.clear();
  input_buffer_.clear();
  snake_created_at_ms_.clear();
  dirty_snake_ids_.clear();
  deleted_snake_ids_.clear();
  pending_snake_events_.clear();
  world_chunk_dirty_ = false;

  int max_snake_id = 0;
  for (const auto& ss : stored_snakes) {
    Snake s;
    s.id = ToInt(ss.snake_id);
    s.user_id = ToInt(ss.owner_user_id);
    s.alive = ss.alive;
    s.dir = static_cast<Dir>(ss.direction);
    s.paused = ss.paused;
    s.grow = 0;
    s.color = ss.color.empty() ? ColorForUser(s.user_id) : ss.color;
    s.body = DecodeBody(ss.body_compact);
    if (s.body.empty()) {
      s.body.push_back({ss.head_x, ss.head_y});
    }

    if (!s.body.empty() && s.id > 0 && s.user_id > 0 && s.alive) {
      snakes_.push_back(std::move(s));
      snake_created_at_ms_[snakes_.back().id] = ss.created_at;
      max_snake_id = std::max(max_snake_id, snakes_.back().id);
    }
  }
  next_snake_id_ = max_snake_id + 1;

  if (world_chunk.has_value()) {
    foods_ = DecodeFoods(world_chunk->food_state);
    world_version_ = world_chunk->version;
    if (world_chunk->width > 0) width_ = world_chunk->width;
    if (world_chunk->height > 0) height_ = world_chunk->height;
  }

  SpawnSystem::Run(snakes_, foods_, food_count_, width_, height_, rng_);
  ResolveOverlapsOnStartLocked();

  if (!world_chunk.has_value()) {
    // First boot with empty DB needs an initial world row.
    world_chunk_dirty_ = true;
    ++world_version_;
  }
}

void World::Tick() {
  std::lock_guard<std::mutex> lock(mu_);

  std::unordered_map<int, std::pair<Dir, bool>> before_dir_pause;
  before_dir_pause.reserve(snakes_.size());
  for (const auto& s : snakes_) {
    before_dir_pause[s.id] = {s.dir, s.paused};
  }

  MovementSystem::Run(snakes_, input_buffer_, width_, height_);

  std::vector<CollisionEvent> events;
  events.reserve(8);
  bool food_changed = false;
  CollisionSystem::Run(snakes_, foods_, width_, height_, rng_, events, food_changed);

  SpawnSystem::Run(snakes_, foods_, food_count_, width_, height_, rng_);

  const int64_t created_at = 0;
  for (const auto& e : events) {
    PushSnakeEventLocked(e, created_at);
    if (e.snake_id > 0) MarkSnakeDirtyLocked(e.snake_id);
    if (e.other_snake_id > 0) MarkSnakeDirtyLocked(e.other_snake_id);
    if (e.event_type == "DEATH" && e.snake_id > 0) {
      deleted_snake_ids_.insert(e.snake_id);
      dirty_snake_ids_.erase(e.snake_id);
    }
  }

  for (const auto& s : snakes_) {
    auto it = before_dir_pause.find(s.id);
    if (it == before_dir_pause.end()) continue;
    if (it->second.first != s.dir || it->second.second != s.paused) {
      MarkSnakeDirtyLocked(s.id);
    }
  }

  if (food_changed || !events.empty()) {
    world_chunk_dirty_ = true;
    ++world_version_;
  }

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

  const int64_t now = static_cast<int64_t>(tick_);
  snake_created_at_ms_[s.id] = now;
  MarkSnakeDirtyLocked(s.id);

  CollisionEvent ev;
  ev.event_type = "SPAWN";
  ev.snake_id = s.id;
  ev.x = p.x;
  ev.y = p.y;
  ev.delta_length = 1;
  PushSnakeEventLocked(ev, now);

  return s.id;
}

PersistenceDelta World::DrainPersistenceDelta(int64_t ts_ms) {
  std::lock_guard<std::mutex> lock(mu_);

  PersistenceDelta delta;

  delta.delete_snake_ids.reserve(deleted_snake_ids_.size());
  for (int sid : deleted_snake_ids_) {
    delta.delete_snake_ids.push_back(std::to_string(sid));
    snake_created_at_ms_.erase(sid);
  }
  deleted_snake_ids_.clear();

  for (int sid : dirty_snake_ids_) {
    const Snake* s = FindSnakeLocked(sid);
    if (!s) continue;

    storage::Snake out;
    out.snake_id = std::to_string(s->id);
    out.owner_user_id = std::to_string(s->user_id);
    out.alive = s->alive;
    out.head_x = s->body.empty() ? 0 : s->body[0].x;
    out.head_y = s->body.empty() ? 0 : s->body[0].y;
    out.direction = static_cast<int>(s->dir);
    out.paused = s->paused;
    out.length_k = static_cast<int>(s->body.size());
    out.body_compact = EncodeBody(s->body);
    out.color = s->color;
    out.created_at = snake_created_at_ms_.count(sid) ? snake_created_at_ms_[sid] : ts_ms;
    out.updated_at = ts_ms;

    for (auto it = pending_snake_events_.rbegin(); it != pending_snake_events_.rend(); ++it) {
      if (it->snake_id == out.snake_id) {
        out.last_event_id = it->event_id;
        break;
      }
    }

    delta.upsert_snakes.push_back(std::move(out));
  }
  dirty_snake_ids_.clear();

  if (world_chunk_dirty_) {
    storage::WorldChunk chunk;
    chunk.chunk_id = "main";
    chunk.width = width_;
    chunk.height = height_;
    chunk.obstacles = "[]";
    chunk.food_state = EncodeFoods(foods_);
    chunk.version = world_version_;
    chunk.updated_at = ts_ms;
    delta.upsert_world_chunk = chunk;
    world_chunk_dirty_ = false;
  }

  delta.snake_events = std::move(pending_snake_events_);
  pending_snake_events_.clear();
  for (auto& e : delta.snake_events) {
    if (e.created_at <= 0) e.created_at = ts_ms;
    if (e.world_version <= 0) e.world_version = world_version_;
  }

  return delta;
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

std::string World::EncodeBody(const std::vector<Vec2>& body) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < body.size(); ++i) {
    out << "[" << body[i].x << "," << body[i].y << "]";
    if (i + 1 < body.size()) out << ",";
  }
  out << "]";
  return out.str();
}

std::vector<Vec2> World::DecodeBody(const std::string& body_compact) {
  std::vector<Vec2> out;
  size_t i = 0;
  auto skip_ws = [&]() {
    while (i < body_compact.size() && std::isspace(static_cast<unsigned char>(body_compact[i]))) ++i;
  };
  auto read_int = [&](int& value) -> bool {
    skip_ws();
    if (i >= body_compact.size()) return false;
    size_t start = i;
    if (body_compact[i] == '-') ++i;
    while (i < body_compact.size() && std::isdigit(static_cast<unsigned char>(body_compact[i]))) ++i;
    if (i == start || (i == start + 1 && body_compact[start] == '-')) return false;
    try {
      value = std::stoi(body_compact.substr(start, i - start));
      return true;
    } catch (...) {
      return false;
    }
  };

  skip_ws();
  if (i >= body_compact.size() || body_compact[i] != '[') return out;
  ++i;
  while (i < body_compact.size()) {
    skip_ws();
    if (i < body_compact.size() && body_compact[i] == ']') break;
    if (i >= body_compact.size() || body_compact[i] != '[') break;
    ++i;

    int x = 0;
    int y = 0;
    if (!read_int(x)) break;
    skip_ws();
    if (i >= body_compact.size() || body_compact[i] != ',') break;
    ++i;
    if (!read_int(y)) break;
    skip_ws();
    if (i >= body_compact.size() || body_compact[i] != ']') break;
    ++i;

    out.push_back({x, y});
    skip_ws();
    if (i < body_compact.size() && body_compact[i] == ',') ++i;
  }

  return out;
}

std::string World::EncodeFoods(const std::vector<Food>& foods) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < foods.size(); ++i) {
    out << "[" << foods[i].x << "," << foods[i].y << "]";
    if (i + 1 < foods.size()) out << ",";
  }
  out << "]";
  return out.str();
}

std::vector<Food> World::DecodeFoods(const std::string& food_state) {
  std::vector<Food> out;
  for (const auto& p : DecodeBody(food_state)) {
    out.push_back(Food{p.x, p.y});
  }
  return out;
}

Snake* World::FindSnakeLocked(int snake_id) {
  for (auto& s : snakes_) {
    if (s.id == snake_id) return &s;
  }
  return nullptr;
}

const Snake* World::FindSnakeLocked(int snake_id) const {
  for (const auto& s : snakes_) {
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
      MarkSnakeDirtyLocked(s.id);
    }

    for (const auto& c : s.body) {
      occupied.insert(key(c));
    }
  }
}

void World::MarkSnakeDirtyLocked(int snake_id) {
  if (snake_id > 0 && !deleted_snake_ids_.count(snake_id)) {
    dirty_snake_ids_.insert(snake_id);
  }
}

void World::PushSnakeEventLocked(const CollisionEvent& e, int64_t created_at) {
  if (e.snake_id <= 0 || e.event_type.empty()) return;

  storage::SnakeEvent out;
  out.snake_id = std::to_string(e.snake_id);
  out.event_id = std::to_string(created_at) + "#" + std::to_string(tick_) + "#" + e.event_type + "#" + std::to_string(pending_snake_events_.size());
  out.event_type = e.event_type;
  out.x = e.x;
  out.y = e.y;
  if (e.other_snake_id > 0) out.other_snake_id = std::to_string(e.other_snake_id);
  out.delta_length = e.delta_length;
  out.tick_number = tick_;
  out.world_version = world_version_;
  out.created_at = created_at;
  pending_snake_events_.push_back(std::move(out));
}

}  // namespace world
