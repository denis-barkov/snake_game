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
  std::string economy_period_user_table;
};

class DynamoStorage : public IStorage {
 public:
  explicit DynamoStorage(DynamoConfig cfg);

  std::vector<User> ListUsers() override;
  std::optional<User> GetUserByUsername(const std::string& username) override;
  std::optional<User> GetUserByGoogleSubject(const std::string& google_subject_id) override;
  bool CompanyNameExistsNormalized(const std::string& company_name_normalized,
                                   const std::string& exclude_user_id = "") override;
  std::optional<User> GetUserById(const std::string& user_id) override;
  bool PutUser(const User& u) override;
  bool UpdateUserLastSeenWorldVersion(const std::string& user_id, const std::string& version) override;
  bool DeleteUserById(const std::string& user_id) override;
  bool UpdateUserBalance(const std::string& user_id, int64_t new_balance) override;
  bool IncrementUserBalance(const std::string& user_id, int64_t delta_balance) override;
  bool BorrowCellsAndTrackPeriod(const std::string& user_id,
                                 int64_t amount,
                                 const std::string& period_key,
                                 int64_t& out_balance_mi) override;

  std::vector<Snake> ListSnakes() override;
  std::optional<Snake> GetSnakeById(const std::string& snake_id) override;
  bool SnakeNameExistsNormalized(const std::string& snake_name_normalized,
                                 const std::string& exclude_snake_id = "") override;
  bool PutSnake(const Snake& s) override;
  bool DeleteSnake(const std::string& snake_id) override;
  bool DeleteSnakeEventsBySnakeId(const std::string& snake_id) override;
  bool AttachCellsToSnake(const std::string& user_id,
                          const std::string& snake_id,
                          int64_t amount,
                          int64_t& out_balance_mi,
                          int64_t& out_length_k) override;

  std::optional<WorldChunk> GetWorldChunk(const std::string& chunk_id) override;
  bool PutWorldChunk(const WorldChunk& chunk) override;

  bool AppendSnakeEvent(const SnakeEvent& e) override;

  std::optional<Settings> GetSettings(const std::string& settings_id = "global") override;
  bool PutSettings(const Settings& settings) override;

  std::optional<EconomyParams> GetEconomyParams() override;
  std::optional<EconomyParams> GetEconomyParamsActive() override;
  bool PutEconomyParams(const EconomyParams& p) override;
  bool PutEconomyParamsActiveAndVersioned(const EconomyParams& p, const std::string& updated_by) override;
  std::optional<EconomyPeriod> GetEconomyPeriod(const std::string& period_key) override;
  bool PutEconomyPeriod(const EconomyPeriod& p) override;
  bool IncrementEconomyPeriodDeltaMBuy(const std::string& period_key, int64_t delta_m_buy) override;
  bool IncrementEconomyPeriodRaw(const std::string& period_key,
                                 int64_t harvested_food_delta,
                                 int64_t movement_ticks_delta) override;
  std::optional<EconomyPeriodUser> GetEconomyPeriodUser(const std::string& period_key,
                                                        const std::string& user_id) override;
  bool PutEconomyPeriodUser(const EconomyPeriodUser& p) override;
  bool IncrementEconomyPeriodUserRaw(const std::string& period_key,
                                     const std::string& user_id,
                                     int64_t harvested_food_delta,
                                     int64_t movement_ticks_delta) override;
  std::vector<EconomyPeriodUser> ListEconomyPeriodUsers(const std::string& period_key) override;
  bool IncrementSystemReserve(int64_t delta_cells) override;

  bool HealthCheck() override;
  bool ResetForDev() override;

 private:
  DynamoConfig cfg_;
  std::shared_ptr<Aws::DynamoDB::DynamoDBClient> client_;
};

}  // namespace storage
