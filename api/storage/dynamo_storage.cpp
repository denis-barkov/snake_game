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
      u.role = GetString(item, "role", "player");
      u.created_at = GetInt64(item, "created_at");
      u.company_name = GetString(item, "company_name");
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
  if (!out.IsSuccess()) return std::nullopt;
  const auto& items = out.GetResult().GetItems();
  if (items.empty()) return std::nullopt;

  User u;
  u.user_id = GetString(items[0], "user_id");
  u.username = GetString(items[0], "username");
  u.password_hash = GetString(items[0], "password_hash");
  u.balance_mi = GetInt64(items[0], "balance_mi");
  u.role = GetString(items[0], "role", "player");
  u.created_at = GetInt64(items[0], "created_at");
  u.company_name = GetString(items[0], "company_name");
  return u;
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
  u.role = GetString(item, "role", "player");
  u.created_at = GetInt64(item, "created_at");
  u.company_name = GetString(item, "company_name");
  return u;
}

bool DynamoStorage::PutUser(const User& u) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.AddItem("user_id", S(u.user_id));
  req.AddItem("username", S(u.username));
  req.AddItem("password_hash", S(u.password_hash));
  req.AddItem("balance_mi", N(u.balance_mi));
  req.AddItem("role", S(u.role.empty() ? "player" : u.role));
  req.AddItem("created_at", N(u.created_at));
  if (!u.company_name.empty()) req.AddItem("company_name", S(u.company_name));
  return client_->PutItem(req).IsSuccess();
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
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.snakes_table.c_str());
  req.AddItem("snake_id", S(s.snake_id));
  req.AddItem("owner_user_id", S(s.owner_user_id));
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

bool DynamoStorage::DeleteSnake(const std::string& snake_id) {
  Aws::DynamoDB::Model::DeleteItemRequest req;
  req.SetTableName(cfg_.snakes_table.c_str());
  req.AddKey("snake_id", S(snake_id));
  return client_->DeleteItem(req).IsSuccess();
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
         delete_by_scan(cfg_.economy_params_table, "params_id", std::nullopt) &&
         delete_by_scan(cfg_.settings_table, "settings_id", std::nullopt) &&
         delete_by_scan(cfg_.world_chunks_table, "chunk_id", std::nullopt) &&
         delete_by_scan(cfg_.snakes_table, "snake_id", std::nullopt) &&
         delete_by_scan(cfg_.users_table, "user_id", std::nullopt);
}

}  // namespace storage
