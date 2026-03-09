#include "dynamo_storage.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <iostream>
#include <sstream>
#include <thread>

#include <aws/core/client/ClientConfiguration.h>
#include <aws/dynamodb/model/AttributeValue.h>
#include <aws/dynamodb/model/DeleteItemRequest.h>
#include <aws/dynamodb/model/DescribeTableRequest.h>
#include <aws/dynamodb/model/GetItemRequest.h>
#include <aws/dynamodb/model/PutItemRequest.h>
#include <aws/dynamodb/model/QueryRequest.h>
#include <aws/dynamodb/model/ScanRequest.h>
#include <aws/dynamodb/model/TransactWriteItem.h>
#include <aws/dynamodb/model/TransactWriteItemsRequest.h>
#include <aws/dynamodb/model/UpdateItemRequest.h>

namespace storage {
namespace {

using Aws::DynamoDB::Model::AttributeValue;
using Aws::Map;
using Aws::String;

std::string GetString(const Map<String, AttributeValue>& item, const char* key, const std::string& def = "") {
  auto it = item.find(key);
  if (it == item.end()) return def;
  return it->second.GetS().c_str();
}

int64_t GetInt64(const Map<String, AttributeValue>& item, const char* key, int64_t def = 0) {
  auto it = item.find(key);
  if (it == item.end()) return def;
  const auto& n = it->second.GetN();
  if (n.empty()) return def;
  try {
    return std::stoll(n.c_str());
  } catch (...) {
    return def;
  }
}

double GetDouble(const Map<String, AttributeValue>& item, const char* key, double def = 0.0) {
  auto it = item.find(key);
  if (it == item.end()) return def;
  const auto& n = it->second.GetN();
  if (n.empty()) return def;
  try {
    return std::stod(n.c_str());
  } catch (...) {
    return def;
  }
}

bool GetBool(const Map<String, AttributeValue>& item, const char* key, bool def = false) {
  auto it = item.find(key);
  if (it == item.end()) return def;
  return it->second.GetBool();
}

AttributeValue S(const std::string& v) {
  AttributeValue a;
  a.SetS(v.c_str());
  return a;
}

AttributeValue N(int64_t v) {
  AttributeValue a;
  a.SetN(std::to_string(v).c_str());
  return a;
}

AttributeValue D(double v) {
  AttributeValue a;
  std::ostringstream out;
  out << v;
  a.SetN(out.str().c_str());
  return a;
}

AttributeValue B(bool v) {
  AttributeValue a;
  a.SetBool(v);
  return a;
}

EconomyParams LoadEconomyParamsFromItem(const Map<String, AttributeValue>& item) {
  EconomyParams p;
  p.version = static_cast<int>(GetInt64(item, "version", 1));
  p.k_land = static_cast<int>(GetInt64(item, "k_land", 24));
  p.a_productivity = GetDouble(item, "a_productivity", 1.0);
  p.v_velocity = GetDouble(item, "v_velocity", 2.0);
  p.food_spawn_target = static_cast<int>(GetInt64(item, "food_spawn_target", 1));
  p.alpha_bootstrap_default = GetDouble(item, "alpha_bootstrap_default", 0.5);
  p.m_gov_reserve = GetInt64(item, "m_gov_reserve", 400);
  p.cap_delta_m = GetInt64(item, "cap_delta_m", 5000);
  p.delta_m_issue = GetInt64(item, "delta_m_issue", 0);
  p.delta_k_obs = GetInt64(item, "delta_k_obs", 0);
  p.updated_at = GetInt64(item, "updated_at", 0);
  p.updated_by = GetString(item, "updated_by");
  return p;
}

std::string EncodeBody(const std::vector<std::pair<int, int>>& body) {
  std::ostringstream out;
  out << "[";
  for (size_t i = 0; i < body.size(); ++i) {
    out << "[" << body[i].first << "," << body[i].second << "]";
    if (i + 1 < body.size()) out << ",";
  }
  out << "]";
  return out.str();
}

std::vector<std::pair<int, int>> DecodeBody(const std::string& json) {
  std::vector<std::pair<int, int>> out;
  size_t i = 0;
  auto skip_ws = [&]() {
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
  };
  auto read_int = [&](int& value) -> bool {
    skip_ws();
    if (i >= json.size()) return false;
    size_t start = i;
    if (json[i] == '-') ++i;
    while (i < json.size() && std::isdigit(static_cast<unsigned char>(json[i]))) ++i;
    if (i == start || (i == start + 1 && json[start] == '-')) return false;
    try {
      value = std::stoi(json.substr(start, i - start));
      return true;
    } catch (...) {
      return false;
    }
  };

  skip_ws();
  if (i >= json.size() || json[i] != '[') return out;
  ++i;
  while (i < json.size()) {
    skip_ws();
    if (i < json.size() && json[i] == ']') break;
    if (i >= json.size() || json[i] != '[') break;
    ++i;

    int x = 0;
    int y = 0;
    if (!read_int(x)) break;
    skip_ws();
    if (i >= json.size() || json[i] != ',') break;
    ++i;
    if (!read_int(y)) break;
    skip_ws();
    if (i >= json.size() || json[i] != ']') break;
    ++i;
    out.push_back({x, y});

    skip_ws();
    if (i < json.size() && json[i] == ',') ++i;
  }

  return out;
}

}  // namespace

DynamoStorage::DynamoStorage(DynamoConfig cfg) : cfg_(std::move(cfg)) {
  Aws::Client::ClientConfiguration cc;
  cc.region = cfg_.region.c_str();
  if (!cfg_.endpoint.empty()) {
    cc.endpointOverride = cfg_.endpoint.c_str();
    cc.scheme = Aws::Http::Scheme::HTTP;
  }
  client_ = std::make_shared<Aws::DynamoDB::DynamoDBClient>(cc);
}

std::vector<User> DynamoStorage::ListUsers() {
  std::vector<User> out;
  Aws::DynamoDB::Model::ScanRequest req;
  req.SetTableName(cfg_.users_table.c_str());

  while (true) {
    auto res = client_->Scan(req);
    if (!res.IsSuccess()) break;
    for (const auto& item : res.GetResult().GetItems()) {
      User u;
      u.user_id = GetString(item, "user_id");
      u.username = GetString(item, "username");
      u.password_hash = GetString(item, "password_hash");
      u.balance_mi = GetInt64(item, "balance_mi");
      u.debt_principal = GetInt64(item, "debt_principal", 0);
      u.debt_interest_rate = GetDouble(item, "debt_interest_rate", 0.0);
      u.debt_accrued_interest = GetInt64(item, "debt_accrued_interest", 0);
      u.role = GetString(item, "role", "player");
      u.created_at = GetInt64(item, "created_at");
      u.updated_at = GetInt64(item, "updated_at", u.created_at);
      u.company_name = GetString(item, "company_name");
      u.company_name_normalized = GetString(item, "company_name_normalized");
      u.last_seen_world_version = GetString(item, "last_seen_world_version");
      u.auth_provider = GetString(item, "auth_provider", "local");
      u.google_subject_id = GetString(item, "google_subject_id");
      u.onboarding_completed = GetBool(item, "onboarding_completed", false);
      u.starter_snake_id = GetString(item, "starter_snake_id");
      u.account_status = GetString(item, "account_status", "active");
      out.push_back(std::move(u));
    }

    const auto& lek = res.GetResult().GetLastEvaluatedKey();
    if (lek.empty()) break;
    req.SetExclusiveStartKey(lek);
  }

  return out;
}

std::optional<User> DynamoStorage::GetUserByUsername(const std::string& username) {
  Aws::DynamoDB::Model::QueryRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.SetIndexName("gsi_username");
  req.SetKeyConditionExpression("username = :u");
  req.SetLimit(1);
  req.AddExpressionAttributeValues(":u", S(username));

  auto out = client_->Query(req);
  const auto materialize_user = [&](const Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>& item)
      -> std::optional<User> {
    if (item.empty()) return std::nullopt;
    User u;
    u.user_id = GetString(item, "user_id");
    u.username = GetString(item, "username");
    u.password_hash = GetString(item, "password_hash");
    u.balance_mi = GetInt64(item, "balance_mi");
    u.debt_principal = GetInt64(item, "debt_principal", 0);
    u.debt_interest_rate = GetDouble(item, "debt_interest_rate", 0.0);
    u.debt_accrued_interest = GetInt64(item, "debt_accrued_interest", 0);
    u.role = GetString(item, "role", "player");
    u.created_at = GetInt64(item, "created_at");
    u.updated_at = GetInt64(item, "updated_at", u.created_at);
    u.company_name = GetString(item, "company_name");
    u.company_name_normalized = GetString(item, "company_name_normalized");
    u.last_seen_world_version = GetString(item, "last_seen_world_version");
    u.auth_provider = GetString(item, "auth_provider", "local");
    u.google_subject_id = GetString(item, "google_subject_id");
    u.onboarding_completed = GetBool(item, "onboarding_completed", false);
    u.starter_snake_id = GetString(item, "starter_snake_id");
    u.account_status = GetString(item, "account_status", "active");
    return u;
  };

  if (out.IsSuccess()) {
    const auto& items = out.GetResult().GetItems();
    if (!items.empty()) {
      return materialize_user(items[0]);
    }
    return std::nullopt;
  }

  // Compatibility fallback: if the users table is missing gsi_username,
  // login still works via scan-based lookup. This keeps local/prod usable
  // across historical table definitions.
  Aws::DynamoDB::Model::ScanRequest scan;
  scan.SetTableName(cfg_.users_table.c_str());
  scan.SetFilterExpression("username = :u");
  scan.AddExpressionAttributeValues(":u", S(username));
  scan.SetLimit(1);
  auto scan_out = client_->Scan(scan);
  if (!scan_out.IsSuccess()) return std::nullopt;
  const auto& scanned = scan_out.GetResult().GetItems();
  if (scanned.empty()) return std::nullopt;
  return materialize_user(scanned[0]);
}

std::optional<User> DynamoStorage::GetUserById(const std::string& user_id) {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.AddKey("user_id", S(user_id));

  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

  User u;
  u.user_id = GetString(item, "user_id");
  u.username = GetString(item, "username");
  u.password_hash = GetString(item, "password_hash");
  u.balance_mi = GetInt64(item, "balance_mi");
  u.debt_principal = GetInt64(item, "debt_principal", 0);
  u.debt_interest_rate = GetDouble(item, "debt_interest_rate", 0.0);
  u.debt_accrued_interest = GetInt64(item, "debt_accrued_interest", 0);
  u.role = GetString(item, "role", "player");
  u.created_at = GetInt64(item, "created_at");
  u.updated_at = GetInt64(item, "updated_at", u.created_at);
  u.company_name = GetString(item, "company_name");
  u.company_name_normalized = GetString(item, "company_name_normalized");
  u.last_seen_world_version = GetString(item, "last_seen_world_version");
  u.auth_provider = GetString(item, "auth_provider", "local");
  u.google_subject_id = GetString(item, "google_subject_id");
  u.onboarding_completed = GetBool(item, "onboarding_completed", false);
  u.starter_snake_id = GetString(item, "starter_snake_id");
  u.account_status = GetString(item, "account_status", "active");
  return u;
}

std::optional<User> DynamoStorage::GetUserByGoogleSubject(const std::string& google_subject_id) {
  if (google_subject_id.empty()) return std::nullopt;
  Aws::DynamoDB::Model::ScanRequest scan;
  scan.SetTableName(cfg_.users_table.c_str());
  scan.SetFilterExpression("google_subject_id = :g");
  scan.AddExpressionAttributeValues(":g", S(google_subject_id));
  scan.SetLimit(1);
  auto out = client_->Scan(scan);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& items = out.GetResult().GetItems();
  if (items.empty()) return std::nullopt;
  const auto& item = items[0];
  User u;
  u.user_id = GetString(item, "user_id");
  u.username = GetString(item, "username");
  u.password_hash = GetString(item, "password_hash");
  u.balance_mi = GetInt64(item, "balance_mi");
  u.debt_principal = GetInt64(item, "debt_principal", 0);
  u.debt_interest_rate = GetDouble(item, "debt_interest_rate", 0.0);
  u.debt_accrued_interest = GetInt64(item, "debt_accrued_interest", 0);
  u.role = GetString(item, "role", "player");
  u.created_at = GetInt64(item, "created_at");
  u.updated_at = GetInt64(item, "updated_at", u.created_at);
  u.company_name = GetString(item, "company_name");
  u.company_name_normalized = GetString(item, "company_name_normalized");
  u.last_seen_world_version = GetString(item, "last_seen_world_version");
  u.auth_provider = GetString(item, "auth_provider", "local");
  u.google_subject_id = GetString(item, "google_subject_id");
  u.onboarding_completed = GetBool(item, "onboarding_completed", false);
  u.starter_snake_id = GetString(item, "starter_snake_id");
  u.account_status = GetString(item, "account_status", "active");
  return u;
}

bool DynamoStorage::CompanyNameExistsNormalized(const std::string& company_name_normalized,
                                                const std::string& exclude_user_id) {
  if (company_name_normalized.empty()) return false;
  Aws::DynamoDB::Model::ScanRequest scan;
  scan.SetTableName(cfg_.users_table.c_str());
  scan.SetFilterExpression("company_name_normalized = :n");
  scan.AddExpressionAttributeValues(":n", S(company_name_normalized));
  auto out = client_->Scan(scan);
  if (!out.IsSuccess()) return false;
  for (const auto& item : out.GetResult().GetItems()) {
    const auto uid = GetString(item, "user_id");
    if (!exclude_user_id.empty() && uid == exclude_user_id) continue;
    return true;
  }
  return false;
}

bool DynamoStorage::PutUser(const User& u) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.AddItem("user_id", S(u.user_id));
  req.AddItem("username", S(u.username));
  req.AddItem("password_hash", S(u.password_hash));
  req.AddItem("balance_mi", N(u.balance_mi));
  req.AddItem("debt_principal", N(std::max<int64_t>(0, u.debt_principal)));
  req.AddItem("debt_interest_rate", D(std::max(0.0, std::min(1.0, u.debt_interest_rate))));
  req.AddItem("debt_accrued_interest", N(std::max<int64_t>(0, u.debt_accrued_interest)));
  req.AddItem("role", S(u.role.empty() ? "player" : u.role));
  req.AddItem("created_at", N(u.created_at));
  req.AddItem("updated_at", N(u.updated_at > 0 ? u.updated_at : u.created_at));
  if (!u.company_name.empty()) req.AddItem("company_name", S(u.company_name));
  if (!u.company_name_normalized.empty()) req.AddItem("company_name_normalized", S(u.company_name_normalized));
  if (!u.last_seen_world_version.empty()) {
    req.AddItem("last_seen_world_version", S(u.last_seen_world_version));
  }
  if (!u.auth_provider.empty()) req.AddItem("auth_provider", S(u.auth_provider));
  if (!u.google_subject_id.empty()) req.AddItem("google_subject_id", S(u.google_subject_id));
  req.AddItem("onboarding_completed", B(u.onboarding_completed));
  if (!u.starter_snake_id.empty()) req.AddItem("starter_snake_id", S(u.starter_snake_id));
  if (!u.account_status.empty()) req.AddItem("account_status", S(u.account_status));
  return client_->PutItem(req).IsSuccess();
}

bool DynamoStorage::UpdateUserLastSeenWorldVersion(const std::string& user_id, const std::string& version) {
  Aws::DynamoDB::Model::UpdateItemRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.AddKey("user_id", S(user_id));
  req.SetUpdateExpression("SET last_seen_world_version = :v");
  req.SetConditionExpression("attribute_exists(user_id)");
  req.AddExpressionAttributeValues(":v", S(version));
  return client_->UpdateItem(req).IsSuccess();
}

bool DynamoStorage::DeleteUserById(const std::string& user_id) {
  Aws::DynamoDB::Model::DeleteItemRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.AddKey("user_id", S(user_id));
  return client_->DeleteItem(req).IsSuccess();
}

bool DynamoStorage::UpdateUserBalance(const std::string& user_id, int64_t new_balance) {
  Aws::DynamoDB::Model::UpdateItemRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.AddKey("user_id", S(user_id));
  req.SetUpdateExpression("SET balance_mi = :b");
  req.AddExpressionAttributeValues(":b", N(new_balance));
  return client_->UpdateItem(req).IsSuccess();
}

bool DynamoStorage::IncrementUserBalance(const std::string& user_id, int64_t delta_balance) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    Aws::DynamoDB::Model::UpdateItemRequest req;
    req.SetTableName(cfg_.users_table.c_str());
    req.AddKey("user_id", S(user_id));
    req.SetUpdateExpression("ADD balance_mi :delta");
    req.AddExpressionAttributeValues(":delta", N(delta_balance));
    auto res = client_->UpdateItem(req);
    if (res.IsSuccess()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
  }
  return false;
}

bool DynamoStorage::BorrowCellsAndTrackPeriod(const std::string& user_id,
                                              int64_t amount,
                                              const std::string& period_key,
                                              int64_t& out_balance_mi,
                                              std::string* out_error_code) {
  if (out_error_code) *out_error_code = "";
  if (amount <= 0) {
    if (out_error_code) *out_error_code = "invalid_amount";
    return false;
  }

  std::string treasury_params_id = "active";
  Aws::DynamoDB::Model::GetItemRequest req_params;
  req_params.SetTableName(cfg_.economy_params_table.c_str());
  req_params.AddKey("params_id", S(treasury_params_id));
  auto params_out = client_->GetItem(req_params);
  auto params_item = params_out.IsSuccess() ? params_out.GetResult().GetItem()
                                            : Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>{};
  if (params_item.empty()) {
    treasury_params_id = "global";
    Aws::DynamoDB::Model::GetItemRequest req_legacy;
    req_legacy.SetTableName(cfg_.economy_params_table.c_str());
    req_legacy.AddKey("params_id", S(treasury_params_id));
    params_out = client_->GetItem(req_legacy);
    params_item = params_out.IsSuccess() ? params_out.GetResult().GetItem()
                                         : Aws::Map<Aws::String, Aws::DynamoDB::Model::AttributeValue>{};
  }
  if (params_item.empty()) {
    if (out_error_code) *out_error_code = "internal_error";
    return false;
  }
  const auto active_params = LoadEconomyParamsFromItem(params_item);
  if (active_params.m_gov_reserve < amount) {
    if (out_error_code) *out_error_code = "insufficient_treasury";
    return false;
  }

  // Deterministic critical path (no transaction dependency):
  // 1) decrement treasury with a non-negative guard
  // 2) increment user balance
  // 3) best-effort period accounting update
  // 4) rollback treasury on user credit failure
  Aws::DynamoDB::Model::UpdateItemRequest treasury_debit;
  treasury_debit.SetTableName(cfg_.economy_params_table.c_str());
  treasury_debit.AddKey("params_id", S(treasury_params_id));
  treasury_debit.SetConditionExpression("attribute_exists(params_id) AND attribute_exists(m_gov_reserve) AND m_gov_reserve >= :a");
  treasury_debit.SetUpdateExpression("ADD m_gov_reserve :neg");
  treasury_debit.AddExpressionAttributeValues(":a", N(amount));
  treasury_debit.AddExpressionAttributeValues(":neg", N(-amount));
  auto treasury_debit_res = client_->UpdateItem(treasury_debit);
  if (!treasury_debit_res.IsSuccess()) {
    const auto exception_name = treasury_debit_res.GetError().GetExceptionName();
    if (out_error_code) {
      *out_error_code = (exception_name == "ConditionalCheckFailedException")
                            ? "insufficient_treasury"
                            : "persistence_write_failed";
    }
    return false;
  }

  Aws::DynamoDB::Model::UpdateItemRequest user_credit;
  user_credit.SetTableName(cfg_.users_table.c_str());
  user_credit.AddKey("user_id", S(user_id));
  user_credit.SetConditionExpression("attribute_exists(user_id)");
  user_credit.SetUpdateExpression("SET balance_mi = if_not_exists(balance_mi, :z) + :a");
  user_credit.AddExpressionAttributeValues(":z", N(0));
  user_credit.AddExpressionAttributeValues(":a", N(amount));
  auto user_credit_res = client_->UpdateItem(user_credit);
  if (!user_credit_res.IsSuccess()) {
    const auto exception_name = user_credit_res.GetError().GetExceptionName();
    Aws::DynamoDB::Model::UpdateItemRequest treasury_rollback;
    treasury_rollback.SetTableName(cfg_.economy_params_table.c_str());
    treasury_rollback.AddKey("params_id", S(treasury_params_id));
    treasury_rollback.SetUpdateExpression("ADD m_gov_reserve :a");
    treasury_rollback.AddExpressionAttributeValues(":zero", N(0));
    treasury_rollback.AddExpressionAttributeValues(":a", N(amount));
    (void)client_->UpdateItem(treasury_rollback);
    if (out_error_code) {
      *out_error_code = (exception_name == "ConditionalCheckFailedException")
                            ? "unauthorized"
                            : "persistence_write_failed";
    }
    return false;
  }

  if (!period_key.empty()) {
    Aws::DynamoDB::Model::UpdateItemRequest period_update_fallback;
    period_update_fallback.SetTableName(cfg_.economy_period_table.c_str());
    period_update_fallback.AddKey("period_key", S(period_key));
    period_update_fallback.SetUpdateExpression("ADD delta_m_buy :a");
    period_update_fallback.AddExpressionAttributeValues(":a", N(amount));
    (void)client_->UpdateItem(period_update_fallback);
  }

  const auto user = GetUserById(user_id);
  if (!user.has_value()) {
    if (out_error_code) *out_error_code = "internal_error";
    return false;
  }
  out_balance_mi = user->balance_mi;
  return true;
}

std::vector<Snake> DynamoStorage::ListSnakes() {
  std::vector<Snake> out;
  Aws::DynamoDB::Model::ScanRequest req;
  req.SetTableName(cfg_.snakes_table.c_str());

  while (true) {
    auto res = client_->Scan(req);
    if (!res.IsSuccess()) break;
    for (const auto& item : res.GetResult().GetItems()) {
      Snake s;
      s.snake_id = GetString(item, "snake_id");
      s.owner_user_id = GetString(item, "owner_user_id");
      s.snake_name = GetString(item, "snake_name");
      s.snake_name_normalized = GetString(item, "snake_name_normalized");
      s.alive = GetBool(item, "alive", true);
      s.is_on_field = GetBool(item, "is_on_field", s.alive);
      s.head_x = static_cast<int>(GetInt64(item, "head_x", 0));
      s.head_y = static_cast<int>(GetInt64(item, "head_y", 0));
      s.direction = static_cast<int>(GetInt64(item, "direction", 0));
      s.paused = GetBool(item, "paused", false);
      s.length_k = static_cast<int>(GetInt64(item, "length_k", 0));
      s.body_compact = GetString(item, "body_compact", "[]");
      s.color = GetString(item, "color", "#00ff00");
      s.last_event_id = GetString(item, "last_event_id");
      s.created_at = GetInt64(item, "created_at", 0);
      s.updated_at = GetInt64(item, "updated_at", 0);
      out.push_back(std::move(s));
    }

    const auto& lek = res.GetResult().GetLastEvaluatedKey();
    if (lek.empty()) break;
    req.SetExclusiveStartKey(lek);
  }

  return out;
}

std::optional<Snake> DynamoStorage::GetSnakeById(const std::string& snake_id) {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.snakes_table.c_str());
  req.AddKey("snake_id", S(snake_id));

  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

  Snake s;
  s.snake_id = GetString(item, "snake_id");
  s.owner_user_id = GetString(item, "owner_user_id");
  s.snake_name = GetString(item, "snake_name");
  s.snake_name_normalized = GetString(item, "snake_name_normalized");
  s.alive = GetBool(item, "alive", true);
  s.is_on_field = GetBool(item, "is_on_field", s.alive);
  s.head_x = static_cast<int>(GetInt64(item, "head_x", 0));
  s.head_y = static_cast<int>(GetInt64(item, "head_y", 0));
  s.direction = static_cast<int>(GetInt64(item, "direction", 0));
  s.paused = GetBool(item, "paused", false);
  s.length_k = static_cast<int>(GetInt64(item, "length_k", 0));
  s.body_compact = GetString(item, "body_compact", "[]");
  s.color = GetString(item, "color", "#00ff00");
  s.last_event_id = GetString(item, "last_event_id");
  s.created_at = GetInt64(item, "created_at", 0);
  s.updated_at = GetInt64(item, "updated_at", 0);
  return s;
}

bool DynamoStorage::PutSnake(const Snake& s) {
  if (!s.snake_name_normalized.empty() &&
      SnakeNameExistsNormalized(s.snake_name_normalized, s.snake_id)) {
    return false;
  }

  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.snakes_table.c_str());
  req.AddItem("snake_id", S(s.snake_id));
  req.AddItem("owner_user_id", S(s.owner_user_id));
  if (!s.snake_name.empty()) req.AddItem("snake_name", S(s.snake_name));
  if (!s.snake_name_normalized.empty()) req.AddItem("snake_name_normalized", S(s.snake_name_normalized));
  req.AddItem("alive", B(s.alive));
  req.AddItem("is_on_field", B(s.is_on_field));
  req.AddItem("head_x", N(s.head_x));
  req.AddItem("head_y", N(s.head_y));
  req.AddItem("direction", N(s.direction));
  req.AddItem("paused", B(s.paused));
  req.AddItem("length_k", N(s.length_k));
  req.AddItem("body_compact", S(s.body_compact));
  req.AddItem("color", S(s.color));
  if (!s.last_event_id.empty()) req.AddItem("last_event_id", S(s.last_event_id));
  req.AddItem("created_at", N(s.created_at));
  req.AddItem("updated_at", N(s.updated_at));
  return client_->PutItem(req).IsSuccess();
}

bool DynamoStorage::SnakeNameExistsNormalized(const std::string& snake_name_normalized,
                                              const std::string& exclude_snake_id) {
  if (snake_name_normalized.empty()) return false;
  Aws::DynamoDB::Model::QueryRequest q;
  q.SetTableName(cfg_.snakes_table.c_str());
  q.SetIndexName("gsi_snake_name_normalized");
  q.SetKeyConditionExpression("snake_name_normalized = :n");
  q.AddExpressionAttributeValues(":n", S(snake_name_normalized));
  q.SetLimit(10);
  auto out_q = client_->Query(q);
  if (out_q.IsSuccess()) {
    for (const auto& item : out_q.GetResult().GetItems()) {
      const auto sid = GetString(item, "snake_id");
      if (!exclude_snake_id.empty() && sid == exclude_snake_id) continue;
      return true;
    }
    return false;
  }

  // Backward-compatible fallback while older tables without the GSI still exist.
  Aws::DynamoDB::Model::ScanRequest scan;
  scan.SetTableName(cfg_.snakes_table.c_str());
  scan.SetFilterExpression("snake_name_normalized = :n");
  scan.AddExpressionAttributeValues(":n", S(snake_name_normalized));
  auto out_s = client_->Scan(scan);
  if (!out_s.IsSuccess()) return false;
  for (const auto& item : out_s.GetResult().GetItems()) {
    const auto sid = GetString(item, "snake_id");
    if (!exclude_snake_id.empty() && sid == exclude_snake_id) continue;
    return true;
  }
  return false;
}

bool DynamoStorage::DeleteSnake(const std::string& snake_id) {
  Aws::DynamoDB::Model::DeleteItemRequest req;
  req.SetTableName(cfg_.snakes_table.c_str());
  req.AddKey("snake_id", S(snake_id));
  return client_->DeleteItem(req).IsSuccess();
}

bool DynamoStorage::DeleteSnakeEventsBySnakeId(const std::string& snake_id) {
  if (snake_id.empty()) return true;
  Aws::DynamoDB::Model::QueryRequest q;
  q.SetTableName(cfg_.snake_events_table.c_str());
  q.SetKeyConditionExpression("snake_id = :s");
  q.AddExpressionAttributeValues(":s", S(snake_id));
  while (true) {
    auto out = client_->Query(q);
    if (!out.IsSuccess()) return false;
    for (const auto& item : out.GetResult().GetItems()) {
      const auto event_id = GetString(item, "event_id");
      if (event_id.empty()) continue;
      Aws::DynamoDB::Model::DeleteItemRequest del;
      del.SetTableName(cfg_.snake_events_table.c_str());
      del.AddKey("snake_id", S(snake_id));
      del.AddKey("event_id", S(event_id));
      if (!client_->DeleteItem(del).IsSuccess()) return false;
    }
    const auto& lek = out.GetResult().GetLastEvaluatedKey();
    if (lek.empty()) break;
    q.SetExclusiveStartKey(lek);
  }
  return true;
}

bool DynamoStorage::AttachCellsToSnake(const std::string& user_id,
                                       const std::string& snake_id,
                                       int64_t amount,
                                       int64_t& out_balance_mi,
                                       int64_t& out_length_k) {
  if (amount <= 0) return false;

  Aws::DynamoDB::Model::TransactWriteItemsRequest tx;

  Aws::DynamoDB::Model::Update user_update;
  user_update.SetTableName(cfg_.users_table.c_str());
  user_update.AddKey("user_id", S(user_id));
  user_update.SetConditionExpression("attribute_exists(user_id) AND attribute_exists(balance_mi) AND balance_mi >= :a");
  user_update.SetUpdateExpression("SET balance_mi = balance_mi - :a");
  user_update.AddExpressionAttributeValues(":a", N(amount));

  Aws::DynamoDB::Model::TransactWriteItem user_item;
  user_item.SetUpdate(user_update);
  tx.AddTransactItems(user_item);

  const int64_t ts = static_cast<int64_t>(time(nullptr));
  Aws::DynamoDB::Model::Update snake_update;
  snake_update.SetTableName(cfg_.snakes_table.c_str());
  snake_update.AddKey("snake_id", S(snake_id));
  // Keep transaction ownership-safe but do not require alive/on_field flags in DB.
  // Runtime world validation controls whether a snake can be actively manipulated.
  snake_update.SetConditionExpression("attribute_exists(snake_id) AND owner_user_id = :uid");
  snake_update.SetUpdateExpression("SET length_k = if_not_exists(length_k, :z) + :a, updated_at = :ts");
  snake_update.AddExpressionAttributeValues(":uid", S(user_id));
  snake_update.AddExpressionAttributeValues(":z", N(0));
  snake_update.AddExpressionAttributeValues(":a", N(amount));
  snake_update.AddExpressionAttributeValues(":ts", N(ts));

  Aws::DynamoDB::Model::TransactWriteItem snake_item;
  snake_item.SetUpdate(snake_update);
  tx.AddTransactItems(snake_item);

  auto tx_res = client_->TransactWriteItems(tx);
  if (tx_res.IsSuccess()) {
    const auto user = GetUserById(user_id);
    const auto snake = GetSnakeById(snake_id);
    if (!user.has_value() || !snake.has_value()) return false;
    out_balance_mi = user->balance_mi;
    out_length_k = snake->length_k;
    return true;
  }

  // Fallback path for environments where transactions are unavailable or flaky:
  // 1) conditionally debit user balance
  // 2) conditionally grow owned snake
  // 3) rollback debit on step 2 failure
  Aws::DynamoDB::Model::UpdateItemRequest user_debit;
  user_debit.SetTableName(cfg_.users_table.c_str());
  user_debit.AddKey("user_id", S(user_id));
  user_debit.SetConditionExpression("attribute_exists(user_id) AND attribute_exists(balance_mi) AND balance_mi >= :a");
  user_debit.SetUpdateExpression("SET balance_mi = balance_mi - :a");
  user_debit.AddExpressionAttributeValues(":a", N(amount));
  auto debit_res = client_->UpdateItem(user_debit);
  if (!debit_res.IsSuccess()) return false;

  const int64_t ts_fallback = static_cast<int64_t>(time(nullptr));
  Aws::DynamoDB::Model::UpdateItemRequest snake_grow;
  snake_grow.SetTableName(cfg_.snakes_table.c_str());
  snake_grow.AddKey("snake_id", S(snake_id));
  snake_grow.SetConditionExpression("attribute_exists(snake_id) AND owner_user_id = :uid");
  snake_grow.SetUpdateExpression("SET length_k = if_not_exists(length_k, :z) + :a, updated_at = :ts");
  snake_grow.AddExpressionAttributeValues(":uid", S(user_id));
  snake_grow.AddExpressionAttributeValues(":z", N(0));
  snake_grow.AddExpressionAttributeValues(":a", N(amount));
  snake_grow.AddExpressionAttributeValues(":ts", N(ts_fallback));
  auto grow_res = client_->UpdateItem(snake_grow);
  if (!grow_res.IsSuccess()) {
    // Best-effort compensation to preserve user funds if snake update failed.
    Aws::DynamoDB::Model::UpdateItemRequest rollback_user;
    rollback_user.SetTableName(cfg_.users_table.c_str());
    rollback_user.AddKey("user_id", S(user_id));
    rollback_user.SetUpdateExpression("SET balance_mi = if_not_exists(balance_mi, :zero) + :a");
    rollback_user.AddExpressionAttributeValues(":zero", N(0));
    rollback_user.AddExpressionAttributeValues(":a", N(amount));
    (void)client_->UpdateItem(rollback_user);
    return false;
  }

  const auto user = GetUserById(user_id);
  const auto snake = GetSnakeById(snake_id);
  if (!user.has_value() || !snake.has_value()) return false;
  out_balance_mi = user->balance_mi;
  out_length_k = snake->length_k;
  return true;
}

std::optional<WorldChunk> DynamoStorage::GetWorldChunk(const std::string& chunk_id) {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.world_chunks_table.c_str());
  req.AddKey("chunk_id", S(chunk_id));

  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

  WorldChunk w;
  w.chunk_id = GetString(item, "chunk_id");
  w.width = static_cast<int>(GetInt64(item, "width", 0));
  w.height = static_cast<int>(GetInt64(item, "height", 0));
  w.obstacles = GetString(item, "obstacles", "[]");
  w.food_state = GetString(item, "food_state", "[]");
  w.version = GetInt64(item, "version", 0);
  w.updated_at = GetInt64(item, "updated_at", 0);
  return w;
}

bool DynamoStorage::PutWorldChunk(const WorldChunk& chunk) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.world_chunks_table.c_str());
  req.AddItem("chunk_id", S(chunk.chunk_id));
  req.AddItem("width", N(chunk.width));
  req.AddItem("height", N(chunk.height));
  req.AddItem("obstacles", S(chunk.obstacles));
  req.AddItem("food_state", S(chunk.food_state));
  req.AddItem("version", N(chunk.version));
  req.AddItem("updated_at", N(chunk.updated_at));
  return client_->PutItem(req).IsSuccess();
}

bool DynamoStorage::AppendSnakeEvent(const SnakeEvent& e) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.snake_events_table.c_str());
  req.AddItem("snake_id", S(e.snake_id));
  req.AddItem("event_id", S(e.event_id));
  req.AddItem("event_type", S(e.event_type));
  req.AddItem("x", N(e.x));
  req.AddItem("y", N(e.y));
  if (!e.other_snake_id.empty()) req.AddItem("other_snake_id", S(e.other_snake_id));
  req.AddItem("delta_length", N(e.delta_length));
  req.AddItem("tick_number", N(static_cast<int64_t>(e.tick_number)));
  req.AddItem("world_version", N(e.world_version));
  req.AddItem("created_at", N(e.created_at));
  return client_->PutItem(req).IsSuccess();
}

std::optional<Settings> DynamoStorage::GetSettings(const std::string& settings_id) {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.settings_table.c_str());
  req.AddKey("settings_id", S(settings_id));

  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

  Settings s;
  s.settings_id = GetString(item, "settings_id", "global");
  s.tick_hz = static_cast<int>(GetInt64(item, "tick_hz", 10));
  s.spectator_hz = static_cast<int>(GetInt64(item, "spectator_hz", 10));
  s.max_snakes_per_user = static_cast<int>(GetInt64(item, "max_snakes_per_user", 3));
  s.feature_flags_json = GetString(item, "feature_flags", "{}");
  s.economy_refs_json = GetString(item, "economy_refs", "{}");
  s.updated_at = GetInt64(item, "updated_at", 0);
  return s;
}

bool DynamoStorage::PutSettings(const Settings& settings) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.settings_table.c_str());
  req.AddItem("settings_id", S(settings.settings_id));
  req.AddItem("tick_hz", N(settings.tick_hz));
  req.AddItem("spectator_hz", N(settings.spectator_hz));
  req.AddItem("max_snakes_per_user", N(settings.max_snakes_per_user));
  req.AddItem("feature_flags", S(settings.feature_flags_json));
  req.AddItem("economy_refs", S(settings.economy_refs_json));
  req.AddItem("updated_at", N(settings.updated_at));
  return client_->PutItem(req).IsSuccess();
}

std::optional<EconomyParams> DynamoStorage::GetEconomyParams() {
  return GetEconomyParamsActive();
}

std::optional<EconomyParams> DynamoStorage::GetEconomyParamsActive() {
  Aws::DynamoDB::Model::GetItemRequest req_active;
  req_active.SetTableName(cfg_.economy_params_table.c_str());
  req_active.AddKey("params_id", S("active"));

  auto out = client_->GetItem(req_active);
  if (!out.IsSuccess()) return std::nullopt;
  auto item = out.GetResult().GetItem();
  if (!item.empty()) return LoadEconomyParamsFromItem(item);

  // Backward compatibility for older rows keyed as "global".
  Aws::DynamoDB::Model::GetItemRequest req_global;
  req_global.SetTableName(cfg_.economy_params_table.c_str());
  req_global.AddKey("params_id", S("global"));
  out = client_->GetItem(req_global);
  if (!out.IsSuccess()) return std::nullopt;
  item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;
  return LoadEconomyParamsFromItem(item);
}

bool DynamoStorage::PutEconomyParams(const EconomyParams& p) {
  return PutEconomyParamsActiveAndVersioned(p, p.updated_by.empty() ? "system" : p.updated_by);
}

bool DynamoStorage::PutEconomyParamsActiveAndVersioned(const EconomyParams& p, const std::string& updated_by) {
  int next_version = std::max(1, p.version);
  const auto active = GetEconomyParamsActive();
  if (active.has_value() && next_version <= active->version) {
    next_version = active->version + 1;
  }
  const int64_t updated_at = p.updated_at > 0 ? p.updated_at : static_cast<int64_t>(time(nullptr));

  // History row first.
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.economy_params_table.c_str());
  req.AddItem("params_id", S("ver#" + std::to_string(next_version)));
  req.AddItem("version", N(next_version));
  req.AddItem("k_land", N(p.k_land));
  req.AddItem("a_productivity", D(p.a_productivity));
  req.AddItem("v_velocity", D(p.v_velocity));
  req.AddItem("food_spawn_target", N(std::max(1, p.food_spawn_target)));
  req.AddItem("alpha_bootstrap_default", D(std::max(0.05, std::min(0.95, p.alpha_bootstrap_default))));
  req.AddItem("m_gov_reserve", N(p.m_gov_reserve));
  req.AddItem("cap_delta_m", N(p.cap_delta_m));
  req.AddItem("delta_m_issue", N(p.delta_m_issue));
  req.AddItem("delta_k_obs", N(p.delta_k_obs));
  req.AddItem("updated_at", N(updated_at));
  req.AddItem("updated_by", S(updated_by));
  if (!client_->PutItem(req).IsSuccess()) return false;

  // Active row points to latest values.
  req = Aws::DynamoDB::Model::PutItemRequest();
  req.SetTableName(cfg_.economy_params_table.c_str());
  req.AddItem("params_id", S("active"));
  req.AddItem("version", N(next_version));
  req.AddItem("k_land", N(p.k_land));
  req.AddItem("a_productivity", D(p.a_productivity));
  req.AddItem("v_velocity", D(p.v_velocity));
  req.AddItem("food_spawn_target", N(std::max(1, p.food_spawn_target)));
  req.AddItem("alpha_bootstrap_default", D(std::max(0.05, std::min(0.95, p.alpha_bootstrap_default))));
  req.AddItem("m_gov_reserve", N(p.m_gov_reserve));
  req.AddItem("cap_delta_m", N(p.cap_delta_m));
  req.AddItem("delta_m_issue", N(p.delta_m_issue));
  req.AddItem("delta_k_obs", N(p.delta_k_obs));
  req.AddItem("updated_at", N(updated_at));
  req.AddItem("updated_by", S(updated_by));
  return client_->PutItem(req).IsSuccess();
}

std::optional<EconomyPeriod> DynamoStorage::GetEconomyPeriod(const std::string& period_key) {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.economy_period_table.c_str());
  req.AddKey("period_key", S(period_key));

  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

  EconomyPeriod p;
  p.period_key = period_key;
  p.harvested_food = GetInt64(item, "harvested_food", 0);
  p.real_output = GetInt64(item, "real_output", p.harvested_food);
  p.movement_ticks = GetInt64(item, "movement_ticks", 0);
  p.total_output = GetInt64(item, "total_output", 0);
  p.total_capital = GetInt64(item, "total_capital", 0);
  p.total_labor = GetInt64(item, "total_labor", 0);
  p.capital_share = GetDouble(item, "capital_share", 0.5);
  p.productivity_index = GetDouble(item, "productivity_index", 0.0);
  p.money_supply = GetInt64(item, "money_supply", 0);
  p.price_index = GetDouble(item, "price_index", 0.0);
  p.inflation_rate = GetDouble(item, "inflation_rate", 0.0);
  p.price_index_valid = GetBool(item, "price_index_valid", false);
  p.inflation_valid = GetBool(item, "inflation_valid", false);
  p.treasury_balance = GetInt64(item, "treasury_balance", 0);
  p.alpha_bootstrap = GetBool(item, "alpha_bootstrap", false);
  p.is_finalized = GetBool(item, "is_finalized", false);
  p.finalized_at = GetInt64(item, "finalized_at", 0);
  p.snapshot_status = GetString(item, "snapshot_status", "live_unfinalized");
  p.period_ends_in_seconds = GetInt64(item, "period_ends_in_seconds", 0);
  p.delta_m_buy = GetInt64(item, "delta_m_buy", 0);
  p.computed_m = GetInt64(item, "computed_m", 0);
  p.computed_k = GetInt64(item, "computed_k", 0);
  p.computed_y = GetInt64(item, "computed_y", 0);
  p.computed_p = GetInt64(item, "computed_p", 0);
  p.computed_pi = GetInt64(item, "computed_pi", 0);
  p.computed_world_area = GetInt64(item, "computed_world_area", 0);
  p.computed_white = GetInt64(item, "computed_white", 0);
  p.computed_at = GetInt64(item, "computed_at", 0);
  return p;
}

bool DynamoStorage::PutEconomyPeriod(const EconomyPeriod& p) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.economy_period_table.c_str());
  req.AddItem("period_key", S(p.period_key));
  req.AddItem("harvested_food", N(p.harvested_food));
  req.AddItem("real_output", N(p.real_output));
  req.AddItem("movement_ticks", N(p.movement_ticks));
  req.AddItem("total_output", N(p.total_output));
  req.AddItem("total_capital", N(p.total_capital));
  req.AddItem("total_labor", N(p.total_labor));
  req.AddItem("capital_share", D(p.capital_share));
  req.AddItem("productivity_index", D(p.productivity_index));
  req.AddItem("money_supply", N(p.money_supply));
  req.AddItem("price_index", D(p.price_index));
  req.AddItem("inflation_rate", D(p.inflation_rate));
  req.AddItem("price_index_valid", B(p.price_index_valid));
  req.AddItem("inflation_valid", B(p.inflation_valid));
  req.AddItem("treasury_balance", N(p.treasury_balance));
  req.AddItem("alpha_bootstrap", B(p.alpha_bootstrap));
  req.AddItem("is_finalized", B(p.is_finalized));
  req.AddItem("finalized_at", N(p.finalized_at));
  req.AddItem("snapshot_status", S(p.snapshot_status));
  req.AddItem("period_ends_in_seconds", N(p.period_ends_in_seconds));
  req.AddItem("delta_m_buy", N(p.delta_m_buy));
  req.AddItem("computed_m", N(p.computed_m));
  req.AddItem("computed_k", N(p.computed_k));
  req.AddItem("computed_y", N(p.computed_y));
  req.AddItem("computed_p", N(p.computed_p));
  req.AddItem("computed_pi", N(p.computed_pi));
  req.AddItem("computed_world_area", N(p.computed_world_area));
  req.AddItem("computed_white", N(p.computed_white));
  req.AddItem("computed_at", N(p.computed_at));
  return client_->PutItem(req).IsSuccess();
}

bool DynamoStorage::IncrementEconomyPeriodDeltaMBuy(const std::string& period_key, int64_t delta_m_buy) {
  for (int attempt = 0; attempt < 3; ++attempt) {
    Aws::DynamoDB::Model::UpdateItemRequest req;
    req.SetTableName(cfg_.economy_period_table.c_str());
    req.AddKey("period_key", S(period_key));
    req.SetUpdateExpression("ADD delta_m_buy :delta");
    req.AddExpressionAttributeValues(":delta", N(delta_m_buy));
    auto res = client_->UpdateItem(req);
    if (res.IsSuccess()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50 * (attempt + 1)));
  }
  return false;
}

bool DynamoStorage::IncrementEconomyPeriodRaw(const std::string& period_key,
                                              int64_t harvested_food_delta,
                                              int64_t movement_ticks_delta) {
  if (harvested_food_delta == 0 && movement_ticks_delta == 0) return true;
  Aws::DynamoDB::Model::UpdateItemRequest req;
  req.SetTableName(cfg_.economy_period_table.c_str());
  req.AddKey("period_key", S(period_key));
  req.SetUpdateExpression(
      "ADD harvested_food :h, real_output :h, movement_ticks :m "
      "SET snapshot_status = :status, is_finalized = :f, finalized_at = :z");
  req.AddExpressionAttributeValues(":h", N(harvested_food_delta));
  req.AddExpressionAttributeValues(":m", N(movement_ticks_delta));
  req.AddExpressionAttributeValues(":status", S("live_unfinalized"));
  req.AddExpressionAttributeValues(":f", B(false));
  req.AddExpressionAttributeValues(":z", N(0));
  return client_->UpdateItem(req).IsSuccess();
}

std::optional<EconomyPeriodUser> DynamoStorage::GetEconomyPeriodUser(const std::string& period_key,
                                                                     const std::string& user_id) {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.economy_period_user_table.c_str());
  req.AddKey("period_key", S(period_key));
  req.AddKey("user_id", S(user_id));
  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

  EconomyPeriodUser p;
  p.period_key = period_key;
  p.user_id = user_id;
  p.user_harvested_food = GetInt64(item, "user_harvested_food", 0);
  p.user_real_output = GetInt64(item, "user_real_output", p.user_harvested_food);
  p.user_movement_ticks = GetInt64(item, "user_movement_ticks", 0);
  p.user_output = GetInt64(item, "user_output", 0);
  p.user_capital = GetInt64(item, "user_capital", 0);
  p.user_labor = GetInt64(item, "user_labor", 0);
  p.user_capital_share = GetDouble(item, "user_capital_share", 0.5);
  p.user_productivity = GetDouble(item, "user_productivity", 0.0);
  p.user_market_share = GetDouble(item, "user_market_share", 0.0);
  p.user_storage_balance = GetInt64(item, "user_storage_balance", 0);
  p.alpha_bootstrap = GetBool(item, "alpha_bootstrap", false);
  p.computed_at = GetInt64(item, "computed_at", 0);
  return p;
}

bool DynamoStorage::PutEconomyPeriodUser(const EconomyPeriodUser& p) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.economy_period_user_table.c_str());
  req.AddItem("period_key", S(p.period_key));
  req.AddItem("user_id", S(p.user_id));
  req.AddItem("user_harvested_food", N(p.user_harvested_food));
  req.AddItem("user_real_output", N(p.user_real_output));
  req.AddItem("user_movement_ticks", N(p.user_movement_ticks));
  req.AddItem("user_output", N(p.user_output));
  req.AddItem("user_capital", N(p.user_capital));
  req.AddItem("user_labor", N(p.user_labor));
  req.AddItem("user_capital_share", D(p.user_capital_share));
  req.AddItem("user_productivity", D(p.user_productivity));
  req.AddItem("user_market_share", D(p.user_market_share));
  req.AddItem("user_storage_balance", N(p.user_storage_balance));
  req.AddItem("alpha_bootstrap", B(p.alpha_bootstrap));
  req.AddItem("computed_at", N(p.computed_at));
  return client_->PutItem(req).IsSuccess();
}

bool DynamoStorage::IncrementEconomyPeriodUserRaw(const std::string& period_key,
                                                  const std::string& user_id,
                                                  int64_t harvested_food_delta,
                                                  int64_t movement_ticks_delta) {
  if (harvested_food_delta == 0 && movement_ticks_delta == 0) return true;
  Aws::DynamoDB::Model::UpdateItemRequest req;
  req.SetTableName(cfg_.economy_period_user_table.c_str());
  req.AddKey("period_key", S(period_key));
  req.AddKey("user_id", S(user_id));
  req.SetUpdateExpression("ADD user_harvested_food :h, user_real_output :h, user_movement_ticks :m");
  req.AddExpressionAttributeValues(":h", N(harvested_food_delta));
  req.AddExpressionAttributeValues(":m", N(movement_ticks_delta));
  return client_->UpdateItem(req).IsSuccess();
}

std::vector<EconomyPeriodUser> DynamoStorage::ListEconomyPeriodUsers(const std::string& period_key) {
  std::vector<EconomyPeriodUser> out_rows;
  Aws::DynamoDB::Model::QueryRequest req;
  req.SetTableName(cfg_.economy_period_user_table.c_str());
  req.SetKeyConditionExpression("period_key = :pk");
  req.AddExpressionAttributeValues(":pk", S(period_key));
  while (true) {
    auto out = client_->Query(req);
    if (!out.IsSuccess()) break;
    for (const auto& item : out.GetResult().GetItems()) {
      EconomyPeriodUser p;
      p.period_key = period_key;
      p.user_id = GetString(item, "user_id");
      p.user_harvested_food = GetInt64(item, "user_harvested_food", 0);
      p.user_real_output = GetInt64(item, "user_real_output", p.user_harvested_food);
      p.user_movement_ticks = GetInt64(item, "user_movement_ticks", 0);
      p.user_output = GetInt64(item, "user_output", 0);
      p.user_capital = GetInt64(item, "user_capital", 0);
      p.user_labor = GetInt64(item, "user_labor", 0);
      p.user_capital_share = GetDouble(item, "user_capital_share", 0.5);
      p.user_productivity = GetDouble(item, "user_productivity", 0.0);
      p.user_market_share = GetDouble(item, "user_market_share", 0.0);
      p.user_storage_balance = GetInt64(item, "user_storage_balance", 0);
      p.alpha_bootstrap = GetBool(item, "alpha_bootstrap", false);
      p.computed_at = GetInt64(item, "computed_at", 0);
      out_rows.push_back(std::move(p));
    }
    if (out.GetResult().GetLastEvaluatedKey().empty()) break;
    req.SetExclusiveStartKey(out.GetResult().GetLastEvaluatedKey());
  }
  return out_rows;
}

bool DynamoStorage::IncrementSystemReserve(int64_t delta_cells) {
  if (delta_cells == 0) return true;
  const int64_t ts = static_cast<int64_t>(time(nullptr));

  auto update_row = [&](const std::string& params_id) {
    Aws::DynamoDB::Model::UpdateItemRequest req;
    req.SetTableName(cfg_.economy_params_table.c_str());
    req.AddKey("params_id", S(params_id));
    req.SetUpdateExpression(
        "SET m_gov_reserve = if_not_exists(m_gov_reserve, :zero) + :delta, "
        "updated_at = :ts, "
        "updated_by = :by");
    req.AddExpressionAttributeValues(":zero", N(0));
    req.AddExpressionAttributeValues(":delta", N(delta_cells));
    req.AddExpressionAttributeValues(":ts", N(ts));
    req.AddExpressionAttributeValues(":by", S("runtime"));
    return client_->UpdateItem(req);
  };

  auto active_res = update_row("active");
  if (active_res.IsSuccess()) return true;
  auto global_res = update_row("global");
  if (global_res.IsSuccess()) return true;
  return false;
}

bool DynamoStorage::HealthCheck() {
  Aws::DynamoDB::Model::DescribeTableRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  auto res = client_->DescribeTable(req);
  if (!res.IsSuccess()) {
    std::cerr << "Dynamo health check failed: " << res.GetError().GetMessage() << "\n";
    return false;
  }
  return true;
}

bool DynamoStorage::ResetForDev() {
  auto delete_by_scan = [&](const std::string& table, const std::string& pk, const std::optional<std::string>& sk) {
    Aws::DynamoDB::Model::ScanRequest scan;
    scan.SetTableName(table.c_str());
    while (true) {
      auto out = client_->Scan(scan);
      if (!out.IsSuccess()) return false;
      for (const auto& item : out.GetResult().GetItems()) {
        Aws::DynamoDB::Model::DeleteItemRequest del;
        del.SetTableName(table.c_str());
        auto it_pk = item.find(pk.c_str());
        if (it_pk == item.end()) continue;
        del.AddKey(pk.c_str(), it_pk->second);
        if (sk.has_value()) {
          auto it_sk = item.find(sk->c_str());
          if (it_sk == item.end()) continue;
          del.AddKey(sk->c_str(), it_sk->second);
        }
        if (!client_->DeleteItem(del).IsSuccess()) return false;
      }
      const auto& lek = out.GetResult().GetLastEvaluatedKey();
      if (lek.empty()) break;
      scan.SetExclusiveStartKey(lek);
    }
    return true;
  };

  return delete_by_scan(cfg_.snake_events_table, "snake_id", std::optional<std::string>("event_id")) &&
         delete_by_scan(cfg_.economy_period_table, "period_key", std::nullopt) &&
         delete_by_scan(cfg_.economy_period_user_table, "period_key", std::optional<std::string>("user_id")) &&
         delete_by_scan(cfg_.economy_params_table, "params_id", std::nullopt) &&
         delete_by_scan(cfg_.settings_table, "settings_id", std::nullopt) &&
         delete_by_scan(cfg_.world_chunks_table, "chunk_id", std::nullopt) &&
         delete_by_scan(cfg_.snakes_table, "snake_id", std::nullopt) &&
         delete_by_scan(cfg_.users_table, "user_id", std::nullopt);
}

}  // namespace storage
