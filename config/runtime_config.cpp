#include "runtime_config.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace {

int clamp_int(int value, int min_v, int max_v) {
  return std::max(min_v, std::min(value, max_v));
}

bool ieq(const std::string& a, const char* b) {
  std::string rhs(b);
  if (a.size() != rhs.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    char ca = static_cast<char>(std::tolower(static_cast<unsigned char>(a[i])));
    char cb = static_cast<char>(std::tolower(static_cast<unsigned char>(rhs[i])));
    if (ca != cb) return false;
  }
  return true;
}

int getenv_int(const char* name, int default_value) {
  const char* v = std::getenv(name);
  if (!v || !*v) return default_value;
  char* end = nullptr;
  long parsed = std::strtol(v, &end, 10);
  if (end == v || *end != '\0') return default_value;
  return static_cast<int>(parsed);
}

double getenv_double(const char* name, double default_value) {
  const char* v = std::getenv(name);
  if (!v || !*v) return default_value;
  char* end = nullptr;
  double parsed = std::strtod(v, &end);
  if (end == v || *end != '\0') return default_value;
  return parsed;
}

bool getenv_bool(const char* name, bool default_value) {
  const char* v = std::getenv(name);
  if (!v || !*v) return default_value;
  std::string s(v);
  if (ieq(s, "1") || ieq(s, "true") || ieq(s, "yes") || ieq(s, "on")) return true;
  if (ieq(s, "0") || ieq(s, "false") || ieq(s, "no") || ieq(s, "off")) return false;
  return default_value;
}

bool has_env(const char* name) {
  const char* v = std::getenv(name);
  return v && *v;
}

std::string getenv_string(const char* name, const std::string& default_value) {
  const char* v = std::getenv(name);
  if (!v || !*v) return default_value;
  return std::string(v);
}

}  // namespace

RuntimeConfig RuntimeConfig::FromEnv() {
  RuntimeConfig cfg;

  cfg.tick_hz = clamp_int(getenv_int("TICK_HZ", cfg.tick_hz), 5, 60);
  cfg.spectator_hz = clamp_int(getenv_int("SPECTATOR_HZ", cfg.spectator_hz), 1, 60);
  cfg.player_hz = clamp_int(getenv_int("PLAYER_HZ", cfg.player_hz), 1, 60);
  cfg.enable_broadcast = getenv_bool("ENABLE_BROADCAST", cfg.enable_broadcast);
  cfg.debug_tps = getenv_bool("DEBUG_TPS", cfg.debug_tps);
  cfg.chunk_size = clamp_int(getenv_int("CHUNK_SIZE", cfg.chunk_size), 8, 1024);
  cfg.aoi_radius = clamp_int(getenv_int("AOI_RADIUS", cfg.aoi_radius), 0, 16);
  cfg.single_chunk_mode = getenv_bool("SINGLE_CHUNK_MODE", cfg.single_chunk_mode);
  cfg.aoi_enabled = getenv_bool("AOI_ENABLED", cfg.aoi_enabled);
  cfg.public_view_enabled = getenv_bool("PUBLIC_VIEW_ENABLED", cfg.public_view_enabled);
  cfg.public_spectator_hz = clamp_int(getenv_int("PUBLIC_SPECTATOR_HZ", cfg.public_spectator_hz), 1, 60);
  cfg.auth_spectator_hz = clamp_int(getenv_int("AUTH_SPECTATOR_HZ", cfg.auth_spectator_hz), 1, 60);
  cfg.public_camera_switch_ticks =
      clamp_int(getenv_int("PUBLIC_CAMERA_SWITCH_TICKS", cfg.public_camera_switch_ticks), 30, 1000000);
  cfg.public_aoi_radius = clamp_int(getenv_int("PUBLIC_AOI_RADIUS", cfg.public_aoi_radius), 0, 16);
  cfg.auth_aoi_radius = clamp_int(getenv_int("AUTH_AOI_RADIUS", cfg.auth_aoi_radius), 0, 16);
  cfg.aoi_pad_chunks = clamp_int(getenv_int("AOI_PAD_CHUNKS", cfg.aoi_pad_chunks), 0, 4);
  cfg.camera_msg_max_hz = clamp_int(getenv_int("CAMERA_MSG_MAX_HZ", cfg.camera_msg_max_hz), 1, 120);
  cfg.max_borrow_per_call = clamp_int(getenv_int("MAX_BORROW_PER_CALL", cfg.max_borrow_per_call), 1, 100000000);
  cfg.food_reward_cells = clamp_int(getenv_int("FOOD_REWARD_CELLS", cfg.food_reward_cells), 1, 1000);
  cfg.resize_threshold = std::max(0.0, std::min(1.0, getenv_double("RESIZE_THRESHOLD", cfg.resize_threshold)));
  cfg.world_aspect_ratio = std::max(0.2, std::min(5.0, getenv_double("WORLD_ASPECT_RATIO", cfg.world_aspect_ratio)));
  cfg.world_mask_mode = getenv_string("WORLD_MASK_MODE", cfg.world_mask_mode);
  cfg.world_mask_seed = getenv_int("WORLD_MASK_SEED", cfg.world_mask_seed);
  cfg.world_mask_style = getenv_string("WORLD_MASK_STYLE", cfg.world_mask_style);
  cfg.econ_period_seconds =
      clamp_int(getenv_int("ECONOMIC_PERIOD_DURATION_SECONDS", getenv_int("ECON_PERIOD_SECONDS", cfg.econ_period_seconds)),
                60, 86400 * 7);
  cfg.econ_period_tz = getenv_string("ECON_PERIOD_TZ", cfg.econ_period_tz);
  const std::string econ_mode = getenv_string("ECONOMIC_PERIOD_MODE", "");
  if (!econ_mode.empty()) {
    if (econ_mode == "prod_midnight_nyc") {
      cfg.econ_period_align = "midnight";
      cfg.econ_period_tz = "America/New_York";
      cfg.econ_period_seconds = 86400;
    } else if (econ_mode == "fixed_seconds") {
      cfg.econ_period_align = "rolling";
    }
  } else {
    cfg.econ_period_align = getenv_string("ECON_PERIOD_ALIGN", cfg.econ_period_align);
  }
  cfg.economy_flush_seconds = clamp_int(getenv_int("ECONOMY_FLUSH_SECONDS", cfg.economy_flush_seconds), 2, 60);
  cfg.economy_period_history_days =
      clamp_int(getenv_int("ECONOMY_PERIOD_HISTORY_DAYS", cfg.economy_period_history_days), 7, 3650);
  cfg.auto_expansion_enabled = getenv_bool("AUTO_EXPANSION_ENABLED", cfg.auto_expansion_enabled);
  cfg.auto_expansion_trigger_ratio =
      std::max(0.1, std::min(1000.0, getenv_double("AUTO_EXPANSION_TRIGGER_RATIO", cfg.auto_expansion_trigger_ratio)));
  cfg.target_spatial_ratio =
      std::max(0.1, std::min(1000.0, getenv_double("TARGET_SPATIAL_RATIO", cfg.target_spatial_ratio)));
  cfg.auto_expansion_checks_per_period =
      clamp_int(getenv_int("AUTO_EXPANSION_CHECKS_PER_PERIOD", cfg.auto_expansion_checks_per_period), 1, 86400);
  cfg.target_lcr = std::max(0.0, std::min(1000.0, getenv_double("TARGET_LCR", cfg.target_lcr)));
  cfg.lcr_stress_threshold =
      std::max(0.0, std::min(1000.0, getenv_double("LCR_STRESS_THRESHOLD", cfg.lcr_stress_threshold)));
  cfg.max_auto_money_growth =
      std::max(0.0, std::min(1.0, getenv_double("MAX_AUTO_MONEY_GROWTH", cfg.max_auto_money_growth)));
  cfg.persistence_profile = getenv_string("PERSISTENCE_PROFILE", cfg.persistence_profile);
  cfg.persistence_sqlite_path = getenv_string("PERSISTENCE_SQLITE_PATH", cfg.persistence_sqlite_path);
  cfg.persistence_sqlite_max_mb =
      clamp_int(getenv_int("PERSISTENCE_SQLITE_MAX_MB", cfg.persistence_sqlite_max_mb), 16, 16384);
  cfg.persistence_sqlite_retention_hours =
      clamp_int(getenv_int("PERSISTENCE_SQLITE_RETENTION_HOURS", cfg.persistence_sqlite_retention_hours), 1, 24 * 365);
  cfg.persistence_flush_chunks_seconds =
      clamp_int(getenv_int("PERSISTENCE_FLUSH_CHUNKS_SECONDS", cfg.persistence_flush_chunks_seconds), 1, 120);
  cfg.persistence_flush_snapshots_seconds =
      clamp_int(getenv_int("PERSISTENCE_FLUSH_SNAPSHOTS_SECONDS", cfg.persistence_flush_snapshots_seconds), 1, 300);
  cfg.persistence_flush_period_deltas_seconds =
      clamp_int(getenv_int("PERSISTENCE_FLUSH_PERIOD_DELTAS_SECONDS", cfg.persistence_flush_period_deltas_seconds), 1, 300);
  cfg.persistence_retry_backoff_ms =
      clamp_int(getenv_int("PERSISTENCE_RETRY_BACKOFF_MS", cfg.persistence_retry_backoff_ms), 10, 60000);
  cfg.persistence_debug_logging = getenv_bool("PERSISTENCE_DEBUG_LOGGING", cfg.persistence_debug_logging);
  cfg.google_auth_enabled = getenv_bool("GOOGLE_AUTH_ENABLED", cfg.google_auth_enabled);
  cfg.google_client_id = getenv_string("GOOGLE_CLIENT_ID", cfg.google_client_id);
  cfg.starter_liquid_assets = static_cast<int64_t>(
      clamp_int(getenv_int("STARTER_LIQUID_ASSETS", static_cast<int>(cfg.starter_liquid_assets)), 1, 1000000));
  cfg.seed_enabled = getenv_bool("SEED_ENABLED", cfg.seed_enabled);
  cfg.seed_config_path = getenv_string("SEED_CONFIG_PATH", cfg.seed_config_path);
  cfg.app_env = getenv_string("APP_ENV", cfg.app_env);
  if (!has_env("DEBUG_TPS")) {
    // Backward compatibility for older deployments that used LOG_HZ.
    cfg.debug_tps = getenv_bool("LOG_HZ", cfg.debug_tps);
  }

  // Backward compatibility with existing env-based deployments.
  if (!has_env("TICK_HZ")) {
    int legacy_tick_ms = getenv_int("SNAKE_TICK_MS", -1);
    if (legacy_tick_ms > 0) {
      int legacy_tick_hz = static_cast<int>(std::lround(1000.0 / static_cast<double>(legacy_tick_ms)));
      cfg.tick_hz = clamp_int(legacy_tick_hz, 5, 60);
    }
  }

  return cfg;
}

int RuntimeConfig::TickIntervalMs() const {
  int interval = static_cast<int>(std::lround(1000.0 / static_cast<double>(tick_hz)));
  return std::max(1, interval);
}

int RuntimeConfig::SpectatorIntervalMs() const {
  int interval = static_cast<int>(std::lround(1000.0 / static_cast<double>(spectator_hz)));
  return std::max(1, interval);
}
