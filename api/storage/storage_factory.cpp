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

std::string RequireEnv(const char* name) {
  const char* v = std::getenv(name);
  if (!v || !*v) {
    throw std::runtime_error(std::string("Missing required environment variable: ") + name);
  }
  return std::string(v);
}

}  // namespace

std::unique_ptr<IStorage> CreateStorageFromEnv() {
  DynamoConfig cfg;
  cfg.endpoint = GetEnvOrDefault("DYNAMO_ENDPOINT", "");
  cfg.region = GetEnvOrDefault("DYNAMO_REGION", GetEnvOrDefault("AWS_REGION", "us-east-1"));
  cfg.users_table = RequireEnv("DYNAMO_TABLE_USERS");
  cfg.snake_checkpoints_table = RequireEnv("DYNAMO_TABLE_SNAKE_CHECKPOINTS");
  cfg.event_ledger_table = RequireEnv("DYNAMO_TABLE_EVENT_LEDGER");
  cfg.settings_table = RequireEnv("DYNAMO_TABLE_SETTINGS");
  return std::make_unique<DynamoStorage>(cfg);
}

}  // namespace storage
