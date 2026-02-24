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
  bool public_view_enabled = true;
  int public_spectator_hz = 10;
  int auth_spectator_hz = 10;
  int public_camera_switch_ticks = 600;
  int public_aoi_radius = 1;
  int auth_aoi_radius = 2;
  int camera_msg_max_hz = 10;

  static RuntimeConfig FromEnv();
  int TickIntervalMs() const;
  int SpectatorIntervalMs() const;
};
