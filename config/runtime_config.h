#pragma once

#include <cstdint>
#include <string>

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
  int aoi_pad_chunks = 1;
  int camera_msg_max_hz = 10;
  int max_borrow_per_call = 1000000;
  int food_reward_cells = 1;
  double resize_threshold = 0.05;
  double world_aspect_ratio = 16.0 / 9.0;
  std::string world_mask_mode = "none";
  int world_mask_seed = 1337;
  std::string world_mask_style = "jagged";
  int econ_period_seconds = 300;
  std::string econ_period_tz = "America/New_York";
  std::string econ_period_align = "rolling";
  int economy_flush_seconds = 10;
  int economy_period_history_days = 90;
  bool auto_expansion_enabled = true;
  double auto_expansion_trigger_ratio = 2.0;
  double target_spatial_ratio = 3.2;
  int auto_expansion_checks_per_period = 48;
  double target_lcr = 1.2;
  double lcr_stress_threshold = 0.7;
  double max_auto_money_growth = 0.08;
  std::string persistence_profile = "minimal";
  std::string persistence_sqlite_path = "/var/lib/snake/persistence.db";
  int persistence_sqlite_max_mb = 256;
  int persistence_sqlite_retention_hours = 72;
  int persistence_flush_chunks_seconds = 2;
  int persistence_flush_snapshots_seconds = 10;
  int persistence_flush_period_deltas_seconds = 10;
  int persistence_retry_backoff_ms = 250;
  bool persistence_debug_logging = false;
  bool google_auth_enabled = false;
  std::string google_client_id;
  int64_t starter_liquid_assets = 25;
  bool auto_seed_on_start = false;

  static RuntimeConfig FromEnv();
  int TickIntervalMs() const;
  int SpectatorIntervalMs() const;
};
