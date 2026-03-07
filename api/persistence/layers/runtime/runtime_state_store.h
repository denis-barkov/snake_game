#pragma once

#include <mutex>
#include <vector>

#include "../../interfaces/interfaces.h"

namespace persistence {

class RuntimeStateStore final : public IRuntimeStore {
 public:
  void RecordIntent(const PersistenceIntent& intent) override;

 private:
  std::mutex mu_;
  std::vector<PersistenceIntent> recent_;  // Lightweight in-process trace buffer.
};

}  // namespace persistence
