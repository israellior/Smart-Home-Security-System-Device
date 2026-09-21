#include "timer_fd.h"

#include <sys/timerfd.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <system_error>

namespace porch {
namespace {

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

void settime(int fd, const itimerspec& spec, int flags) {
  if (::timerfd_settime(fd, flags, &spec, nullptr) != 0) {
    throw_errno("timerfd_settime");
  }
}

}  // namespace

TimerFd::TimerFd() {
  fd_.reset(::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC));
  if (!fd_.valid()) {
    throw_errno("timerfd_create");
  }
}

void TimerFd::arm_at(TimePoint deadline) {
  // CLOCK_MONOTONIC shares its epoch with steady_clock on Linux, which is what
  // makes an absolute deadline translate directly.
  const auto since_epoch =
      std::chrono::duration_cast<std::chrono::nanoseconds>(deadline.time_since_epoch()).count();

  itimerspec spec{};
  spec.it_value.tv_sec = static_cast<std::time_t>(since_epoch / 1'000'000'000);
  spec.it_value.tv_nsec = static_cast<long>(since_epoch % 1'000'000'000);
  // An all-zero it_value disarms the timer rather than firing it.
  if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
    spec.it_value.tv_nsec = 1;
  }
  settime(fd_.get(), spec, TFD_TIMER_ABSTIME);
}

void TimerFd::arm_in(std::chrono::nanoseconds delay) {
  if (delay < std::chrono::nanoseconds::zero()) {
    delay = std::chrono::nanoseconds::zero();
  }
  itimerspec spec{};
  spec.it_value.tv_sec = static_cast<std::time_t>(delay.count() / 1'000'000'000);
  spec.it_value.tv_nsec = static_cast<long>(delay.count() % 1'000'000'000);
  if (spec.it_value.tv_sec == 0 && spec.it_value.tv_nsec == 0) {
    spec.it_value.tv_nsec = 1;  // fire immediately, rather than not at all
  }
  settime(fd_.get(), spec, 0);
}

void TimerFd::disarm() {
  const itimerspec spec{};
  settime(fd_.get(), spec, 0);
}

void TimerFd::drain() {
  std::uint64_t expiries = 0;
  while (::read(fd_.get(), &expiries, sizeof(expiries)) > 0) {
  }
}

}  // namespace porch
