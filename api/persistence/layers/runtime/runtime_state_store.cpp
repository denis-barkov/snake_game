#include "runtime_state_store.h"

#include <cstddef>

namespace persistence {

void RuntimeStateStore::RecordIntent(const PersistenceIntent& intent) {
  std::lock_guard<std::mutex> lock(mu_);
  recent_.push_back(intent);
  if (recent_.size() > 2048) {
    recent_.erase(recent_.begin(), recent_.begin() + static_cast<std::ptrdiff_t>(recent_.size() - 2048));
  }
}

}  // namespace persistence
