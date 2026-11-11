#ifndef RCC0SRD_EVENT_H
#define RCC0SRD_EVENT_H
#include <chrono>
#include <condition_variable>
#include <mutex>

namespace rcc {
class SrdEvent {
 public:
  void start() {
    std::lock_guard<std::mutex> guard(mutex);
    stopped = false;
    pending = true;
  }
  void notify() {
    std::lock_guard<std::mutex> guard(mutex);
    pending = true;
    changed.notify_one();
  }
  bool wait(bool retry) {
    std::unique_lock<std::mutex> guard(mutex);
    changed.wait_for(guard, retry ? std::chrono::milliseconds(10)
                                 : std::chrono::milliseconds(1000),
                     [&] { return pending || stopped; });
    if (stopped) return false;
    pending = false;
    return true;
  }
  void stop() {
    std::lock_guard<std::mutex> guard(mutex);
    stopped = true;
    changed.notify_one();
  }
 private:
  std::mutex mutex;
  std::condition_variable changed;
  bool pending = true, stopped = false;
};
}  // namespace rcc
#endif
