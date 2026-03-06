#pragma once

#include "../interfaces/interfaces.h"

namespace persistence {

class PersistenceRouter final : public IPersistenceRouter {
 public:
  explicit PersistenceRouter(const PersistencePolicyRegistry& registry)
      : registry_(registry) {}

  PersistencePolicy Route(const PersistenceIntent& intent) const override {
    return registry_.Resolve(intent.type);
  }

 private:
  const PersistencePolicyRegistry& registry_;
};

}  // namespace persistence
