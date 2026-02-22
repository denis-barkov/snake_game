#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models.h"

namespace storage {

class IStorage {
 public:
  virtual ~IStorage() = default;

  // Full user listing is used by low-frequency aggregated reads (economy endpoint).
  virtual std::vector<User> ListUsers() = 0;
  virtual std::optional<User> GetUserByUsername(const std::string& username) = 0;
  virtual std::optional<User> GetUserById(const std::string& user_id) = 0;
  virtual bool PutUser(const User& u) = 0;
  virtual bool UpdateUserBalance(const std::string& user_id, int64_t new_balance) = 0;

  virtual std::vector<Snake> ListSnakes() = 0;
  virtual std::optional<Snake> GetSnakeById(const std::string& snake_id) = 0;
  virtual bool PutSnake(const Snake& s) = 0;
  virtual bool DeleteSnake(const std::string& snake_id) = 0;

  virtual std::optional<WorldChunk> GetWorldChunk(const std::string& chunk_id) = 0;
  virtual bool PutWorldChunk(const WorldChunk& chunk) = 0;

  virtual bool AppendSnakeEvent(const SnakeEvent& e) = 0;

  virtual std::optional<Settings> GetSettings(const std::string& settings_id = "global") = 0;
  virtual bool PutSettings(const Settings& settings) = 0;

  virtual std::optional<EconomyParams> GetEconomyParams() = 0;
  virtual bool PutEconomyParams(const EconomyParams& p) = 0;
  virtual std::optional<EconomyPeriod> GetEconomyPeriod(const std::string& period_key) = 0;
  virtual bool PutEconomyPeriod(const EconomyPeriod& p) = 0;

  virtual bool HealthCheck() = 0;
  virtual bool ResetForDev() = 0;
};

}  // namespace storage
