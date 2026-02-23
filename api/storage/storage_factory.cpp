#include "storage_factory.h"

#include <cstdlib>
#include <stdexcept>
#include <string>

#include "dynamo_storage.h"

namespace storage {
namespace {

std::string GetEnvOrDefault(const char* name, const std::string& def) {
  const char* v = std::getenv(name);
  return (v && *v) ? std::string(v) : def;
}

std::string GetEnvAnyOrDefault(const char* first, const char* second, const std::string& def) {
  const char* v1 = std::getenv(first);
  if (v1 && *v1) return std::string(v1);
  const char* v2 = std::getenv(second);
  if (v2 && *v2) return std::string(v2);
  return def;
}

std::string RequireEnv(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    throw std::runtime_error(std::string("Missing required environment variable: ") + name);
  }
  return std::string(v);
}

std::string RequireEnvAny(const char* first, const char* second) {
  const char* v1 = std::getenv(first);
  if (v1 && *v1) return std::string(v1);
  const char* v2 = std::getenv(second);
  if (v2 && *v2) return std::string(v2);
  throw std::runtime_error(
      std::string("Missing required environment variable: ") + first + " (or " + second + ")");
}

}  // namespace

std::unique_ptr<IStorage> CreateStorageFromEnv() {
  DynamoConfig cfg;
  cfg.endpoint = GetEnvAnyOrDefault("DDB_ENDPOINT", "DYNAMO_ENDPOINT", "");
  cfg.region = GetEnvOrDefault("DYNAMO_REGION", GetEnvOrDefault("AWS_REGION", "us-east-1"));
  cfg.users_table = RequireEnvAny("TABLE_USERS", "DYNAMO_TABLE_USERS");
  cfg.snakes_table = RequireEnvAny("TABLE_SNAKES", "DYNAMO_TABLE_SNAKES");
  cfg.world_chunks_table = RequireEnvAny("TABLE_WORLD_CHUNKS", "DYNAMO_TABLE_WORLD_CHUNKS");
  cfg.snake_events_table = RequireEnvAny("TABLE_SNAKE_EVENTS", "DYNAMO_TABLE_SNAKE_EVENTS");
  cfg.settings_table = RequireEnvAny("TABLE_SETTINGS", "DYNAMO_TABLE_SETTINGS");
  cfg.economy_params_table = RequireEnvAny("TABLE_ECONOMY_PARAMS", "DYNAMO_TABLE_ECONOMY_PARAMS");
  cfg.economy_period_table = RequireEnvAny("TABLE_ECONOMY_PERIOD", "DYNAMO_TABLE_ECONOMY_PERIOD");
  return std::make_unique<DynamoStorage>(cfg);
}

}  // namespace storage
