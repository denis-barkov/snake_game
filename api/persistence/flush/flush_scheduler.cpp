#include "flush_scheduler.h"

#include <chrono>
#include <utility>

namespace persistence {

FlushScheduler::FlushScheduler(std::function<void()> tick_fn)
    : tick_fn_(std::move(tick_fn)) {}

FlushScheduler::~FlushScheduler() {
  Stop();
}

void FlushScheduler::Start() {
  if (running_.exchange(true)) return;
  worker_ = std::thread([this] {
    while (running_.load()) {
      tick_fn_();
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
  });
}

void FlushScheduler::Stop() {
  if (!running_.exchange(false)) return;
  if (worker_.joinable()) worker_.join();
}

}  // namespace persistence
