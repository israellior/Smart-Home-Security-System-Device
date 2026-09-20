#pragma once

#include <unistd.h>

#include <utility>

namespace porch {

class UniqueFd {
 public:
  UniqueFd() = default;
  explicit UniqueFd(int fd) : fd_(fd) {}

  UniqueFd(UniqueFd&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

  UniqueFd& operator=(UniqueFd&& other) noexcept {
    if (this != &other) {
      reset(std::exchange(other.fd_, -1));
    }
    return *this;
  }

  UniqueFd(const UniqueFd&) = delete;
  UniqueFd& operator=(const UniqueFd&) = delete;

  ~UniqueFd() { reset(); }

  int get() const { return fd_; }
  bool valid() const { return fd_ >= 0; }

  void reset(int fd = -1) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    fd_ = fd;
  }

 private:
  int fd_ = -1;
};

}  // namespace porch
