#pragma once

#include <cstdint>

struct RuntimeConfig {
  int tick_hz = 20;
  int spectator_hz = 10;
  int player_hz = 20;  // placeholder, unused in Step 1
  bool enable_broadcast = true;
  bool log_hz = true;

  static RuntimeConfig FromEnv();
  int TickIntervalMs() const;
  int SpectatorIntervalMs() const;
};
