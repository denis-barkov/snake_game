#include "dynamo_storage.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <sstream>
#include <unordered_map>

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
  u.created_at = GetInt64(items[0], "created_at");
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
  u.created_at = GetInt64(item, "created_at");
  return u;
}

bool DynamoStorage::PutUser(const User& u) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.users_table.c_str());
  req.AddItem("user_id", S(u.user_id));
  req.AddItem("username", S(u.username));
  req.AddItem("password_hash", S(u.password_hash));
  req.AddItem("balance_mi", N(u.balance_mi));
  req.AddItem("created_at", N(u.created_at));
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

std::vector<SnakeCheckpoint> DynamoStorage::ListLatestSnakeCheckpoints() {
  std::vector<SnakeCheckpoint> out;
  std::unordered_map<std::string, SnakeCheckpoint> latest;

  Aws::DynamoDB::Model::ScanRequest req;
  req.SetTableName(cfg_.snake_checkpoints_table.c_str());

  while (true) {
    auto res = client_->Scan(req);
    if (!res.IsSuccess()) break;

    const auto& items = res.GetResult().GetItems();
    for (const auto& item : items) {
      SnakeCheckpoint cp;
      cp.snake_id = GetString(item, "snake_id");
      cp.owner_user_id = GetString(item, "owner_user_id");
      cp.ts = GetInt64(item, "ts");
      cp.dir = static_cast<int>(GetInt64(item, "dir"));
      cp.paused = GetBool(item, "paused");
      cp.body = DecodeBody(GetString(item, "body", "[]"));
      cp.length = static_cast<int>(GetInt64(item, "length"));
      cp.score = static_cast<int>(GetInt64(item, "score"));
      cp.w = static_cast<int>(GetInt64(item, "w"));
      cp.h = static_cast<int>(GetInt64(item, "h"));

      auto it = latest.find(cp.snake_id);
      if (it == latest.end() || cp.ts > it->second.ts) {
        latest[cp.snake_id] = cp;
      }
    }

    const auto& lek = res.GetResult().GetLastEvaluatedKey();
    if (lek.empty()) break;
    req.SetExclusiveStartKey(lek);
  }

  out.reserve(latest.size());
  for (auto& kv : latest) out.push_back(std::move(kv.second));
  std::sort(out.begin(), out.end(), [](const SnakeCheckpoint& a, const SnakeCheckpoint& b) {
    return a.snake_id < b.snake_id;
  });
  return out;
}

bool DynamoStorage::PutSnakeCheckpoint(const SnakeCheckpoint& cp) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.snake_checkpoints_table.c_str());
  req.AddItem("snake_id", S(cp.snake_id));
  req.AddItem("ts", N(cp.ts));
  req.AddItem("owner_user_id", S(cp.owner_user_id));
  req.AddItem("dir", N(cp.dir));
  req.AddItem("paused", B(cp.paused));
  req.AddItem("body", S(EncodeBody(cp.body)));
  req.AddItem("length", N(cp.length));
  req.AddItem("score", N(cp.score));
  req.AddItem("w", N(cp.w));
  req.AddItem("h", N(cp.h));
  return client_->PutItem(req).IsSuccess();
}

std::optional<EconomyParams> DynamoStorage::GetEconomyParams() {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.settings_table.c_str());
  req.AddKey("pk", S("SYSTEM"));
  req.AddKey("sk", S("ECONOMY_PARAMS"));

  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

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

bool DynamoStorage::PutEconomyParams(const EconomyParams& p) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.settings_table.c_str());
  req.AddItem("pk", S("SYSTEM"));
  req.AddItem("sk", S("ECONOMY_PARAMS"));
  req.AddItem("version", N(p.version));
  req.AddItem("k_land", N(p.k_land));
  req.AddItem("a_productivity", D(p.a_productivity));
  req.AddItem("v_velocity", D(p.v_velocity));
  req.AddItem("m_gov_reserve", N(p.m_gov_reserve));
  req.AddItem("cap_delta_m", N(p.cap_delta_m));
  req.AddItem("delta_m_issue", N(p.delta_m_issue));
  req.AddItem("delta_k_obs", N(p.delta_k_obs));
  req.AddItem("updated_at", N(p.updated_at));
  req.AddItem("updated_by", S(p.updated_by));
  return client_->PutItem(req).IsSuccess();
}

std::optional<EconomyPeriod> DynamoStorage::GetEconomyPeriod(const std::string& period_key) {
  Aws::DynamoDB::Model::GetItemRequest req;
  req.SetTableName(cfg_.settings_table.c_str());
  req.AddKey("pk", S("SYSTEM"));
  req.AddKey("sk", S(std::string("ECONOMY_PERIOD#") + period_key));

  auto out = client_->GetItem(req);
  if (!out.IsSuccess()) return std::nullopt;
  const auto& item = out.GetResult().GetItem();
  if (item.empty()) return std::nullopt;

  EconomyPeriod p;
  p.period_key = period_key;
  p.delta_m_buy = GetInt64(item, "delta_m_buy");
  p.computed_at = GetInt64(item, "computed_at");
  return p;
}

bool DynamoStorage::PutEconomyPeriod(const EconomyPeriod& p) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.settings_table.c_str());
  req.AddItem("pk", S("SYSTEM"));
  req.AddItem("sk", S(std::string("ECONOMY_PERIOD#") + p.period_key));
  req.AddItem("delta_m_buy", N(p.delta_m_buy));
  req.AddItem("computed_at", N(p.computed_at));
  return client_->PutItem(req).IsSuccess();
}

bool DynamoStorage::AppendEvent(const Event& e) {
  Aws::DynamoDB::Model::PutItemRequest req;
  req.SetTableName(cfg_.event_ledger_table.c_str());
  req.AddItem("pk", S(e.pk));
  req.AddItem("sk", S(e.sk));
  req.AddItem("type", S(e.type));
  req.AddItem("payload", S(e.payload_json));
  req.AddItem("created_at", N(e.created_at));
  return client_->PutItem(req).IsSuccess();
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

  return delete_by_scan(cfg_.event_ledger_table, "pk", std::optional<std::string>("sk")) &&
         delete_by_scan(cfg_.settings_table, "pk", std::optional<std::string>("sk")) &&
         delete_by_scan(cfg_.snake_checkpoints_table, "snake_id", std::optional<std::string>("ts")) &&
         delete_by_scan(cfg_.users_table, "user_id", std::nullopt);
}

}  // namespace storage
