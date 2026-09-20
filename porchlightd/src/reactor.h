#pragma once

#include <functional>
#include <map>
#include <optional>

#include "core/clock.h"
#include "unique_fd.h"

namespace porch {

// The whole daemon's I/O: epoll over a set of file descriptors, one timer and
// one signal reader. Single-threaded, and every callback runs on the loop.
class Reactor {
 public:
  using Callback = std::function<void()>;

  Reactor();

  Reactor(const Reactor&) = delete;
  Reactor& operator=(const Reactor&) = delete;

  void watch(int fd, Callback on_readable);
  void unwatch(int fd);

  // One timer for the whole daemon, because the core asks to be woken at one
  // deadline at a time. Passing nothing disarms it.
  void wake_at(std::optional<TimePoint> deadline);
  void on_wake(Callback callback);

  // SIGINT and SIGTERM, arriving as a readable descriptor like everything else.
  void on_signal(Callback callback);

  void run();
  void stop();

 private:
  void drain(int fd);

  UniqueFd epoll_;
  UniqueFd timer_;
  UniqueFd signals_;
  std::map<int, Callback> handlers_;
  Callback on_wake_;
  Callback on_signal_;
  bool running_ = false;
};

}  // namespace porch
