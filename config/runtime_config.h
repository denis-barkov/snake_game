#pragma once

#include <cstdint>

struct RuntimeConfig {
  int tick_hz = 10;
  int spectator_hz = 10;
  int player_hz = 10;  // placeholder, unused in Step 1
  bool enable_broadcast = true;
  bool debug_tps = false;
  int chunk_size = 64;
  int aoi_radius = 1;
  bool single_chunk_mode = true;
  bool aoi_enabled = false;

  static RuntimeConfig FromEnv();
  int TickIntervalMs() const;
  int SpectatorIntervalMs() const;
};
