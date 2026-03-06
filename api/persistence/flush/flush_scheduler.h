#pragma once

#include <atomic>
#include <functional>
#include <thread>

#include "../interfaces/interfaces.h"

namespace persistence {

class FlushScheduler final : public IFlushScheduler {
 public:
  explicit FlushScheduler(std::function<void()> tick_fn);
  ~FlushScheduler() override;

  void Start() override;
  void Stop() override;

 private:
  std::function<void()> tick_fn_;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace persistence
