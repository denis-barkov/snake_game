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
  std::string snakes_table;
  std::string world_chunks_table;
  std::string snake_events_table;
  std::string settings_table;
  std::string economy_params_table;
  std::string economy_period_table;
};

class DynamoStorage : public IStorage {
 public:
  explicit DynamoStorage(DynamoConfig cfg);

  std::optional<User> GetUserByUsername(const std::string& username) override;
  std::optional<User> GetUserById(const std::string& user_id) override;
  bool PutUser(const User& u) override;
  bool UpdateUserBalance(const std::string& user_id, int64_t new_balance) override;

  std::vector<Snake> ListSnakes() override;
  std::optional<Snake> GetSnakeById(const std::string& snake_id) override;
  bool PutSnake(const Snake& s) override;
  bool DeleteSnake(const std::string& snake_id) override;

  std::optional<WorldChunk> GetWorldChunk(const std::string& chunk_id) override;
  bool PutWorldChunk(const WorldChunk& chunk) override;

  bool AppendSnakeEvent(const SnakeEvent& e) override;

  std::optional<Settings> GetSettings(const std::string& settings_id = "global") override;
  bool PutSettings(const Settings& settings) override;

  std::optional<EconomyParams> GetEconomyParams() override;
  bool PutEconomyParams(const EconomyParams& p) override;
  std::optional<EconomyPeriod> GetEconomyPeriod(const std::string& period_key) override;
  bool PutEconomyPeriod(const EconomyPeriod& p) override;

  bool HealthCheck() override;
  bool ResetForDev() override;

 private:
  DynamoConfig cfg_;
  std::shared_ptr<Aws::DynamoDB::DynamoDBClient> client_;
};

}  // namespace storage
