#include "reactor.h"

#include <signal.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/timerfd.h>

#include <cerrno>
#include <cstdint>
#include <system_error>
#include <utility>
#include <vector>

namespace porch {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

constexpr int kMaxEventsPerWait = 16;

}  // namespace

Reactor::Reactor() {
  epoll_.reset(::epoll_create1(EPOLL_CLOEXEC));
  if (!epoll_.valid()) {
    throw_errno("epoll_create1");
  }

  timer_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
  if (!timer_.valid()) {
    throw_errno("timerfd_create");
  }
  watch(timer_.get(), [this] {
    drain(timer_.get());
    if (on_wake_) {
      on_wake_();
    }
  });

  sigset_t mask;
  ::sigemptyset(&mask);
  ::sigaddset(&mask, SIGINT);
  ::sigaddset(&mask, SIGTERM);
  // Blocked process-wide first, so the default disposition never kills us and
  // signalfd is the only reader. The daemon is single-threaded, so this is
  // enough; pthread_sigmask would be needed otherwise.
  if (::sigprocmask(SIG_BLOCK, &mask, nullptr) != 0) {
    throw_errno("sigprocmask");
  }
  signals_.reset(::signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC));
  if (!signals_.valid()) {
    throw_errno("signalfd");
  }
  watch(signals_.get(), [this] {
    drain(signals_.get());
    if (on_signal_) {
      on_signal_();
    }
  });
}

void Reactor::watch(int fd, Callback on_readable) {
  epoll_event event{};
  event.events = EPOLLIN;
  event.data.fd = fd;
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd, &event) != 0) {
    throw_errno("epoll_ctl(ADD)");
  }
  handlers_[fd] = std::move(on_readable);
}

void Reactor::unwatch(int fd) {
  if (handlers_.erase(fd) == 0) {
    return;
  }
  // A descriptor already closed by its owner is gone from the set too, so a
  // failure here is not worth reporting.
  ::epoll_ctl(epoll_.get(), EPOLL_CTL_DEL, fd, nullptr);
}

void Reactor::wake_at(std::optional<TimePoint> deadline) {
  // CLOCK_MONOTONIC shares its epoch with steady_clock on Linux, which is what
  // makes an absolute deadline translate directly.
  itimerspec spec{};
  if (deadline) {
    const auto since_epoch =
        std::chrono::duration_cast<std::chrono::nanoseconds>(deadline->time_since_epoch()).count();
    spec.it_value.tv_sec = static_cast<std::time_t>(since_epoch / 1'000'000'000);
    spec.it_value.tv_nsec = static_cast<long>(since_epoch % 1'000'000'000);
    // An all-zero it_value disarms the timer rather than firing it, so a
    // deadline at exactly the epoch has to be nudged.
    if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
      spec.it_value.tv_nsec = 1;
    }
  }
  if (::timerfd_settime(timer_.get(), TFD_TIMER_ABSTIME, &spec, nullptr) != 0) {
    throw_errno("timerfd_settime");
  }
}

void Reactor::on_wake(Callback callback) { on_wake_ = std::move(callback); }

void Reactor::on_signal(Callback callback) { on_signal_ = std::move(callback); }

void Reactor::drain(int fd) {
  // Both the timer and signalfd are level-triggered, so an unread event would
  // spin the loop.
  std::uint8_t buffer[256];
  while (::read(fd, buffer, sizeof(buffer)) > 0) {
  }
}

void Reactor::run() {
  running_ = true;
  std::vector<epoll_event> events(kMaxEventsPerWait);

  while (running_) {
    const int ready = ::epoll_wait(epoll_.get(), events.data(), kMaxEventsPerWait, -1);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      throw_errno("epoll_wait");
    }

    for (int i = 0; i < ready && running_; ++i) {
      const int fd = events[static_cast<std::size_t>(i)].data.fd;
      const auto handler = handlers_.find(fd);
      if (handler == handlers_.end()) {
        continue;  // an earlier callback in this batch unwatched it
      }
      // Copied, so a callback may unwatch its own descriptor while running.
      Callback callback = handler->second;
      callback();
    }
  }
}

void Reactor::stop() { running_ = false; }

}  // namespace porch
