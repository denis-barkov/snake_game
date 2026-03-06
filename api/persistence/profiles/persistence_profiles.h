#pragma once

#include <string>
#include <map>

#include "../policy/persistence_policy.h"

namespace persistence {

class ProfilePolicyRegistry : public PersistencePolicyRegistry {
 public:
  explicit ProfilePolicyRegistry(std::string profile_name,
                                 int default_retention_hours);

  PersistencePolicy Resolve(IntentType type) const override;
  const std::string& profile_name() const { return profile_name_; }

 private:
  std::string profile_name_;
  std::map<IntentType, PersistencePolicy> policies_;
};

}  // namespace persistence
