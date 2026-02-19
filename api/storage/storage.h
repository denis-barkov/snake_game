#pragma once

#include <optional>
#include <string>
#include <vector>

#include "models.h"

namespace storage {

class IStorage {
 public:
  virtual ~IStorage() = default;

  virtual std::optional<User> GetUserByUsername(const std::string& username) = 0;
  virtual std::optional<User> GetUserById(const std::string& user_id) = 0;
  virtual bool PutUser(const User& u) = 0;
  virtual bool UpdateUserBalance(const std::string& user_id, int64_t new_balance) = 0;

  virtual std::vector<SnakeCheckpoint> ListLatestSnakeCheckpoints() = 0;
  virtual bool PutSnakeCheckpoint(const SnakeCheckpoint& cp) = 0;

  virtual std::optional<EconomyParams> GetEconomyParams() = 0;
  virtual bool PutEconomyParams(const EconomyParams& p) = 0;
  virtual std::optional<EconomyPeriod> GetEconomyPeriod(const std::string& period_key) = 0;
  virtual bool PutEconomyPeriod(const EconomyPeriod& p) = 0;

  virtual bool AppendEvent(const Event& e) = 0;
  virtual bool HealthCheck() = 0;

  // Development helper used by `./snake_server reset`.
  virtual bool ResetForDev() = 0;
};

}  // namespace storage
