#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <aws/dynamodb/DynamoDBClient.h>

#include "storage.h"

namespace storage {

struct DynamoConfig {
  std::string region = "us-east-1";
  std::string endpoint;
  std::string users_table;
  std::string snake_checkpoints_table;
  std::string event_ledger_table;
  std::string settings_table;
};

class DynamoStorage : public IStorage {
 public:
  explicit DynamoStorage(DynamoConfig cfg);

  std::optional<User> GetUserByUsername(const std::string& username) override;
  std::optional<User> GetUserById(const std::string& user_id) override;
  bool PutUser(const User& u) override;
  bool UpdateUserBalance(const std::string& user_id, int64_t new_balance) override;

  std::vector<SnakeCheckpoint> ListLatestSnakeCheckpoints() override;
  bool PutSnakeCheckpoint(const SnakeCheckpoint& cp) override;

  std::optional<EconomyParams> GetEconomyParams() override;
  bool PutEconomyParams(const EconomyParams& p) override;
  std::optional<EconomyPeriod> GetEconomyPeriod(const std::string& period_key) override;
  bool PutEconomyPeriod(const EconomyPeriod& p) override;

  bool AppendEvent(const Event& e) override;
  bool HealthCheck() override;
  bool ResetForDev() override;

 private:
  DynamoConfig cfg_;
  std::shared_ptr<Aws::DynamoDB::DynamoDBClient> client_;
};

}  // namespace storage
