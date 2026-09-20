#pragma once

#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include "core/core.h"

namespace porch::test {

using namespace std::chrono_literals;

class CountingIds : public IdSource {
 public:
  EventId next() override { return "evt-" + std::to_string(++issued_); }
  int issued() const { return issued_; }

 private:
  int issued_ = 0;
};

// Short and round, so a test can say "twenty seconds later" and mean it.
Policy test_policy();

std::string name_of(const Action& action);
std::vector<std::string> names(const std::vector<Action>& actions);

template <class T>
const T* find(const std::vector<Action>& actions) {
  for (const Action& action : actions) {
    if (const T* found = std::get_if<T>(&action)) {
      return found;
    }
  }
  return nullptr;
}

template <class T>
int count(const std::vector<Action>& actions) {
  int found = 0;
  for (const Action& action : actions) {
    if (std::holds_alternative<T>(action)) {
      ++found;
    }
  }
  return found;
}

RecordingFinished good_clip(EventId id, std::chrono::milliseconds duration = 10s,
                            std::string path = "/spool/clip.mp4");
RecordingFinished failed_clip(EventId id);

// A Core plus the clock the test moves by hand.
class Harness {
 public:
  explicit Harness(Policy policy = test_policy());

  std::vector<Action> send(const Event& event);
  std::vector<Action> after(std::chrono::seconds delay, const Event& event = Tick{});
  std::vector<Action> go_online();

  TimePoint now() const { return now_; }
  Core& core() { return core_; }
  const CountingIds& ids() const { return ids_; }
  const Policy& policy() const { return policy_; }

 private:
  Policy policy_;
  CountingIds ids_;
  Core core_;
  // Not the epoch: a deadline computed before time zero would be meaningless.
  TimePoint now_ = TimePoint{} + 1h;
};

}  // namespace porch::test
