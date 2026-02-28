#include "collision_system.h"

#include <algorithm>
#include <set>
#include <unordered_map>
#include <unordered_set>

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

bool IsMoving(const Snake& s) {
  return s.alive && !s.paused && s.dir != Dir::Stop && !s.body.empty();
}

bool IsOpposite(Dir a, Dir b) {
  return OppositeDir(a) == b;
}

void ApplySingleCellLoss(Snake& s,
                         uint64_t tick_id,
                         const std::string& event_type,
                         int other_snake_id,
                         const Vec2& pos,
                         std::vector<CollisionEvent>& events,
                         int delta_system_cells = 0) {
  if (!s.alive) return;
  if (s.last_loss_tick == tick_id) return;
  if (!s.body.empty()) s.body.pop_back();
  s.last_loss_tick = tick_id;
  CollisionEvent ev;
  ev.event_type = event_type;
  ev.snake_id = s.id;
  ev.other_snake_id = other_snake_id;
  ev.x = pos.x;
  ev.y = pos.y;
  ev.delta_length = -1;
  ev.delta_system_cells = delta_system_cells;
  events.push_back(std::move(ev));
  if (s.body.empty()) {
    s.alive = false;
    CollisionEvent death;
    death.event_type = "DEATH";
    death.snake_id = s.id;
    death.x = pos.x;
    death.y = pos.y;
    death.delta_length = -1;
    events.push_back(std::move(death));
  }
}

}  // namespace

void CollisionSystem::Run(std::vector<Snake>& snakes,
                          std::vector<Food>& foods,
                          int width,
                          int height,
                          uint64_t tick_id,
                          int duel_delay_ticks,
                          std::mt19937& rng,
                          std::vector<CollisionEvent>& events,
                          bool& food_changed,
                          const std::function<bool(const Vec2&)>& is_playable) {
  food_changed = false;
  // 1) Resolve pending side-head duels (once).
  std::unordered_set<int> resolved_duels;
  for (auto& s : snakes) {
    if (!s.alive || !s.duel_pending || s.duel_with_id <= 0) continue;
    if (s.duel_resolve_tick > tick_id) continue;
    if (resolved_duels.count(s.id)) continue;
    Snake* other = FindSnakeById(snakes, s.duel_with_id);
    if (!other || !other->alive || !other->duel_pending || other->duel_with_id != s.id || other->duel_resolve_tick > tick_id) {
      s.duel_pending = false;
      s.duel_with_id = 0;
      s.duel_resolve_tick = 0;
      s.paused = false;
      continue;
    }
    std::uniform_int_distribution<int> coin(0, 1);
    Snake* winner = coin(rng) ? &s : other;
    Snake* loser = (winner == &s) ? other : &s;
    const Vec2 impact = winner->body.empty() ? Vec2{} : winner->body.front();

    CollisionEvent win;
    win.event_type = "HEAD_DUEL_WIN";
    win.snake_id = winner->id;
    win.other_snake_id = loser->id;
    win.x = impact.x;
    win.y = impact.y;
    win.credit_user_id = winner->user_id;
    win.delta_user_cells = 1;
    events.push_back(std::move(win));

    ApplySingleCellLoss(*loser, tick_id, "HEAD_DUEL_LOSS", winner->id, impact, events, 0);
    s.duel_pending = false;
    s.duel_with_id = 0;
    s.duel_resolve_tick = 0;
    s.paused = false;
    other->duel_pending = false;
    other->duel_with_id = 0;
    other->duel_resolve_tick = 0;
    other->paused = false;
    resolved_duels.insert(s.id);
    resolved_duels.insert(other->id);
  }

  // 2) Build proposed next-head map for moving snakes.
  struct ProposedMove {
    int snake_id = 0;
    Vec2 current_head{};
    Vec2 next_head{};
    Dir dir = Dir::Stop;
  };
  std::unordered_map<int, ProposedMove> proposed;
  proposed.reserve(snakes.size());
  for (const auto& s : snakes) {
    if (!IsMoving(s)) continue;
    ProposedMove p;
    p.snake_id = s.id;
    p.current_head = s.body.front();
    p.next_head = StepWrapped(s.body.front(), s.dir, width, height);
    p.dir = s.dir;
    proposed[s.id] = p;
  }

  // 3) Priority 1: oncoming head-to-head (same next cell OR cross swap).
  std::unordered_set<int> blocked_move;
  std::set<std::pair<int, int>> oncoming_pairs;
  for (size_t i = 0; i < snakes.size(); ++i) {
    if (!snakes[i].alive) continue;
    auto ita = proposed.find(snakes[i].id);
    if (ita == proposed.end()) continue;
    for (size_t j = i + 1; j < snakes.size(); ++j) {
      if (!snakes[j].alive) continue;
      auto itb = proposed.find(snakes[j].id);
      if (itb == proposed.end()) continue;
      const bool same_next = (ita->second.next_head == itb->second.next_head);
      const bool swap = (ita->second.next_head == itb->second.current_head) &&
                        (itb->second.next_head == ita->second.current_head);
      if (!same_next && !swap) continue;
      const int a = std::min(snakes[i].id, snakes[j].id);
      const int b = std::max(snakes[i].id, snakes[j].id);
      oncoming_pairs.insert({a, b});
    }
  }
  for (const auto& pair : oncoming_pairs) {
    Snake* a = FindSnakeById(snakes, pair.first);
    Snake* b = FindSnakeById(snakes, pair.second);
    if (!a || !b || !a->alive || !b->alive) continue;
    const Vec2 impact = proposed.count(a->id) ? proposed[a->id].next_head : (a->body.empty() ? Vec2{} : a->body.front());
    ApplySingleCellLoss(*a, tick_id, "HEAD_ONCOMING", b->id, impact, events, 1);
    ApplySingleCellLoss(*b, tick_id, "HEAD_ONCOMING", a->id, impact, events, 1);
    a->dir = OppositeDir(a->dir);
    b->dir = OppositeDir(b->dir);
    a->paused = false;
    b->paused = false;
    blocked_move.insert(a->id);
    blocked_move.insert(b->id);
  }

  // 4) Priority 2: side head-hit duel (non-oncoming).
  std::set<std::pair<int, int>> duel_pairs;
  for (const auto& attacker : snakes) {
    if (!attacker.alive || blocked_move.count(attacker.id)) continue;
    auto ita = proposed.find(attacker.id);
    if (ita == proposed.end()) continue;
    for (const auto& defender : snakes) {
      if (!defender.alive || attacker.id == defender.id) continue;
      if (blocked_move.count(defender.id)) continue;
      const Vec2 defender_head = defender.body.empty() ? Vec2{} : defender.body.front();
      if (!(ita->second.next_head == defender_head)) continue;
      const auto itb = proposed.find(defender.id);
      const bool swap = (itb != proposed.end()) &&
                        (ita->second.next_head == itb->second.current_head) &&
                        (itb->second.next_head == ita->second.current_head);
      const bool same_next = (itb != proposed.end()) && (ita->second.next_head == itb->second.next_head);
      if (swap || same_next) continue;
      if (itb != proposed.end() && IsOpposite(ita->second.dir, itb->second.dir)) continue;
      const int a = std::min(attacker.id, defender.id);
      const int b = std::max(attacker.id, defender.id);
      duel_pairs.insert({a, b});
    }
  }
  for (const auto& pair : duel_pairs) {
    Snake* a = FindSnakeById(snakes, pair.first);
    Snake* b = FindSnakeById(snakes, pair.second);
    if (!a || !b || !a->alive || !b->alive) continue;
    a->paused = true;
    b->paused = true;
    a->duel_pending = true;
    b->duel_pending = true;
    a->duel_with_id = b->id;
    b->duel_with_id = a->id;
    const uint64_t resolve_tick = tick_id + static_cast<uint64_t>(std::max(1, duel_delay_ticks));
    a->duel_resolve_tick = resolve_tick;
    b->duel_resolve_tick = resolve_tick;
    blocked_move.insert(a->id);
    blocked_move.insert(b->id);
  }

  // Build tail occupancy for tail-hit checks.
  std::unordered_map<long long, std::vector<int>> tail_owners;
  for (const auto& s : snakes) {
    if (!s.alive || s.body.size() < 2) continue;
    for (size_t i = 1; i < s.body.size(); ++i) {
      tail_owners[CellKey(s.body[i])].push_back(s.id);
    }
  }

  // 5) Priority 3: tail-hit.
  for (const auto& attacker_ref : snakes) {
    if (!attacker_ref.alive || blocked_move.count(attacker_ref.id)) continue;
    auto ita = proposed.find(attacker_ref.id);
    if (ita == proposed.end()) continue;
    auto tail_it = tail_owners.find(CellKey(ita->second.next_head));
    if (tail_it == tail_owners.end()) continue;
    int defender_id = 0;
    for (int candidate : tail_it->second) {
      if (candidate != attacker_ref.id) {
        defender_id = candidate;
        break;
      }
    }
    if (defender_id <= 0) continue;
    Snake* attacker = FindSnakeById(snakes, attacker_ref.id);
    Snake* defender = FindSnakeById(snakes, defender_id);
    if (!attacker || !defender || !attacker->alive || !defender->alive) continue;

    attacker->paused = true;
    const Vec2 impact = ita->second.next_head;
    CollisionEvent bite;
    bite.event_type = "TAIL_BITE";
    bite.snake_id = attacker->id;
    bite.other_snake_id = defender->id;
    bite.x = impact.x;
    bite.y = impact.y;
    bite.credit_user_id = attacker->user_id;
    bite.delta_user_cells = 1;
    events.push_back(std::move(bite));

    ApplySingleCellLoss(*defender, tick_id, "TAIL_BITTEN", attacker->id, impact, events, 0);
    blocked_move.insert(attacker->id);
  }

  // 6) Priority 4/5: self-hit and unplayable-hit.
  for (auto& s : snakes) {
    if (!s.alive || blocked_move.count(s.id)) continue;
    auto itp = proposed.find(s.id);
    if (itp == proposed.end()) continue;

    if (is_playable && !is_playable(itp->second.next_head)) {
      s.paused = true;
      blocked_move.insert(s.id);
      continue;
    }

    bool self_hit = false;
    for (size_t i = 1; i < s.body.size(); ++i) {
      if (s.body[i] == itp->second.next_head) {
        self_hit = true;
        break;
      }
    }
    if (self_hit) {
      ApplySingleCellLoss(s, tick_id, "SELF_COLLISION", 0, itp->second.next_head, events, 0);
      s.paused = true;
      blocked_move.insert(s.id);
    }
  }

  // 7) Commit movement for non-blocked snakes.
  for (auto& s : snakes) {
    if (!s.alive || blocked_move.count(s.id)) continue;
    auto itp = proposed.find(s.id);
    if (itp == proposed.end()) continue;
    s.body.insert(s.body.begin(), itp->second.next_head);
    if (s.grow > 0) {
      --s.grow;
    } else if (!s.body.empty()) {
      s.body.pop_back();
    }
  }

  // 8) Food collision (storage credit handled outside via event type only).
  for (auto& s : snakes) {
    if (!s.alive || s.body.empty()) continue;
    const Vec2 head = s.body.front();
    for (auto& f : foods) {
      if (f.x == head.x && f.y == head.y) {
        CollisionEvent eat;
        eat.event_type = "FOOD_EATEN";
        eat.snake_id = s.id;
        eat.x = head.x;
        eat.y = head.y;
        eat.delta_length = 0;
        events.push_back(std::move(eat));
        Vec2 replacement = SpawnSystem::RandFreeCell(snakes, foods, width, height, rng, is_playable);
        f.x = replacement.x;
        f.y = replacement.y;
        food_changed = true;
      }
    }
  }

  snakes.erase(std::remove_if(snakes.begin(), snakes.end(), [](const Snake& s) { return !s.alive || s.body.empty(); }), snakes.end());
}

}  // namespace world
