#pragma once

#include <cstdint>

struct RuntimeConfig {
  int tick_hz = 10;
  int spectator_hz = 10;
  int player_hz = 10;  // placeholder, unused in Step 1
  bool enable_broadcast = true;
  bool debug_tps = false;

  static RuntimeConfig FromEnv();
  int TickIntervalMs() const;
  int SpectatorIntervalMs() const;
};
