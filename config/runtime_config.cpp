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

}  // namespace

RuntimeConfig RuntimeConfig::FromEnv() {
  RuntimeConfig cfg;

  cfg.tick_hz = clamp_int(getenv_int("TICK_HZ", cfg.tick_hz), 5, 60);
  cfg.spectator_hz = clamp_int(getenv_int("SPECTATOR_HZ", cfg.spectator_hz), 1, 60);
  cfg.player_hz = clamp_int(getenv_int("PLAYER_HZ", cfg.player_hz), 1, 60);
  cfg.enable_broadcast = getenv_bool("ENABLE_BROADCAST", cfg.enable_broadcast);
  cfg.debug_tps = getenv_bool("DEBUG_TPS", cfg.debug_tps);
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
