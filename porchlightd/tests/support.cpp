#include "support.h"

#include <utility>

namespace porch::test {
namespace {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

Policy test_policy() {
  Policy policy;
  policy.clip = 10s;
  policy.min_clip = 2s;
  policy.motion_cooldown = 20s;
  policy.press_dedup_window = 5s;
  policy.ring_led = 3s;
  policy.alert_max_age = 60s;
  policy.max_queued_alerts = 3;
  policy.retry_backoff_initial = 2s;
  policy.retry_backoff_max = 8s;
  // Three clips, since good_clip is a megabyte. Small enough that a test can
  // fill the spool by hand instead of by arithmetic.
  policy.spool_max_bytes = 3ull * 1024 * 1024;
  return policy;
}

std::string name_of(const Action& action) {
  return std::visit(overloaded{
                        [](const SetLed&) { return "SetLed"; },
                        [](const PlayChime&) { return "PlayChime"; },
                        [](const SendAlert&) { return "SendAlert"; },
                        [](const StartRecording&) { return "StartRecording"; },
                        [](const StopRecording&) { return "StopRecording"; },
                        [](const UploadClip&) { return "UploadClip"; },
                        [](const DiscardClip&) { return "DiscardClip"; },
                        [](const StartCall&) { return "StartCall"; },
                        [](const StopCall&) { return "StopCall"; },
                    },
                    action);
}

std::vector<std::string> names(const std::vector<Action>& actions) {
  std::vector<std::string> out;
  out.reserve(actions.size());
  for (const Action& action : actions) {
    out.push_back(name_of(action));
  }
  return out;
}

RecordingFinished good_clip(EventId id, std::chrono::milliseconds duration, std::string path) {
  RecordingFinished finished;
  finished.event_id = std::move(id);
  finished.path = std::move(path);
  finished.ok = true;
  finished.duration = duration;
  finished.bytes = 1024 * 1024;
  return finished;
}

RecordingFinished failed_clip(EventId id) {
  RecordingFinished finished;
  finished.event_id = std::move(id);
  finished.ok = false;
  return finished;
}

Harness::Harness(Policy policy) : policy_(policy), core_(policy, ids_) {}

std::vector<Action> Harness::send(const Event& event) { return core_.handle(event, now_); }

std::vector<Action> Harness::after(std::chrono::seconds delay, const Event& event) {
  now_ += delay;
  return core_.handle(event, now_);
}

std::vector<Action> Harness::go_online() { return send(ServerOnline{}); }

}  // namespace porch::test
