#include "core/core.h"

#include <utility>

namespace porch {
namespace {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

Core::Core(Policy policy, IdSource& ids)
    : policy_(std::move(policy)),
      ids_(ids),
      alerts_(policy_),
      upload_backoff_(policy_.retry_backoff_initial, policy_.retry_backoff_max) {}

std::vector<Action> Core::handle(const Event& event, TimePoint now) {
  std::vector<Action> out;
  lapse(now);

  // Deliberately no generic fallback: a new event type must fail to compile
  // here rather than be silently ignored.
  std::visit(overloaded{
                 [&](const ButtonPressed&) { on_button(now, out); },
                 [&](const MotionDetected&) { on_motion(now, out); },
                 [&](const ViewerRequested& e) { on_viewer(e, out); },
                 [&](const CallEnded& e) { on_call_ended(e); },
                 [&](const RecordingFinished& e) { on_recording_finished(e, out); },
                 [&](const UploadFinished& e) { on_upload_finished(e, now); },
                 [&](const AlertResult& e) { alerts_.on_result(e.event_id, e.kind, e.outcome, now); },
                 [&](const ServerOnline&) { server_online_ = true; },
                 [&](const ServerOffline&) { on_server_offline(now); },
                 [&](const Tick&) {},
                 [&](const Shutdown&) { on_shutdown(out); },
             },
             event);

  pump_alerts(now, out);
  pump_uploads(now, out);
  update_led(out);
  return out;
}

std::optional<TimePoint> Core::next_deadline() const {
  if (shutting_down_) {
    return std::nullopt;
  }
  std::optional<TimePoint> earliest;
  const auto consider = [&earliest](std::optional<TimePoint> candidate) {
    if (candidate && (!earliest || *candidate < *earliest)) {
      earliest = candidate;
    }
  };
  consider(motion_cooldown_until_);
  consider(press_window_until_);
  consider(ring_led_until_);
  consider(alerts_.next_deadline());
  consider(upload_retry_after_);
  return earliest;
}

void Core::lapse(TimePoint now) {
  if (motion_cooldown_until_ && now >= *motion_cooldown_until_) {
    motion_cooldown_until_.reset();
  }
  if (press_window_until_ && now >= *press_window_until_) {
    press_window_until_.reset();
  }
  if (ring_led_until_ && now >= *ring_led_until_) {
    ring_led_until_.reset();
  }
}

void Core::on_motion(TimePoint now, std::vector<Action>& out) {
  if (shutting_down_ || motion_cooldown_until_) {
    return;
  }
  motion_cooldown_until_ = now + policy_.motion_cooldown;

  // A recording already running covers this motion, and a second alert moments
  // after the first one is noise rather than information.
  if (recording_ != RecordingState::Idle) {
    return;
  }
  trigger(Kind::Motion, now, viewers_.empty(), out);
}

void Core::on_button(TimePoint now, std::vector<Action>& out) {
  if (shutting_down_) {
    return;
  }
  // First, and unconditional. The chime is local, so it does not wait for the
  // server, a cooldown, or anything else that can be queued or refused.
  out.push_back(PlayChime{});
  ring_led_until_ = now + policy_.ring_led;

  if (press_window_until_) {
    return;  // one alert per window, but every press rings
  }
  press_window_until_ = now + policy_.press_dedup_window;

  if (recording_ == RecordingState::Active && recording_event_) {
    if (recording_event_->kind == Kind::Motion) {
      // The same event becomes a ring. One id, one clip, one row on the
      // server, notified as a ring because somebody is at the door.
      recording_event_->kind = Kind::Ring;
      alerts_.push({recording_event_->id, Kind::Ring, now});
      pump_alerts(now, out);
    }
    return;  // one recording per event, whichever way it started
  }

  trigger(Kind::Ring, now, viewers_.empty() && recording_ == RecordingState::Idle, out);
}

void Core::on_viewer(const ViewerRequested& event, std::vector<Action>& out) {
  if (shutting_down_) {
    return;
  }
  if (recording_ == RecordingState::Active) {
    out.push_back(StopRecording{});
    recording_ = RecordingState::Stopping;
    deferred_call_ = event.peer;
    return;  // StartCall waits for RecordingFinished, not for StopRecording
  }
  if (recording_ == RecordingState::Stopping) {
    deferred_call_ = event.peer;
    return;
  }
  start_call(event.peer, out);
}

void Core::on_call_ended(const CallEnded& event) {
  viewers_.erase(event.peer);
  if (deferred_call_ == event.peer) {
    deferred_call_.reset();  // gave up while the recorder was still flushing
  }
}

void Core::on_recording_finished(const RecordingFinished& event, std::vector<Action>& out) {
  recording_ = RecordingState::Idle;
  recording_event_.reset();

  const bool worth_keeping =
      event.ok && event.bytes > 0 && event.duration >= policy_.min_clip;
  if (worth_keeping) {
    uploads_.push_back({event.event_id, event.path});
  } else if (!event.path.empty()) {
    out.push_back(DiscardClip{event.event_id, event.path});
  }

  if (deferred_call_) {
    start_call(*deferred_call_, out);
    deferred_call_.reset();
  }
}

void Core::on_upload_finished(const UploadFinished& event, TimePoint now) {
  upload_in_flight_ = false;
  if (!event.ok) {
    upload_retry_after_ = upload_backoff_.fail(now);
    return;
  }
  if (!uploads_.empty() && uploads_.front().event_id == event.event_id) {
    uploads_.pop_front();
  }
  upload_backoff_.reset();
  upload_retry_after_.reset();
}

void Core::on_server_offline(TimePoint now) {
  server_online_ = false;
  // Anything in flight when the link dropped never landed.
  alerts_.link_lost(now);
  if (upload_in_flight_) {
    upload_in_flight_ = false;
    upload_retry_after_ = upload_backoff_.fail(now);
  }
}

void Core::on_shutdown(std::vector<Action>& out) {
  shutting_down_ = true;
  if (recording_ == RecordingState::Active) {
    out.push_back(StopRecording{});
    recording_ = RecordingState::Stopping;
  }
  if (!viewers_.empty()) {
    out.push_back(StopCall{});
    viewers_.clear();
  }
  deferred_call_.reset();
  out.push_back(SetLed{LedPattern::Off});
  led_ = LedPattern::Off;
}

void Core::trigger(Kind kind, TimePoint now, bool with_clip, std::vector<Action>& out) {
  const EventId id = ids_.next();
  alerts_.push({id, kind, now});
  // Before the recording starts, so the server hears about the trigger even if
  // the recorder then fails outright.
  pump_alerts(now, out);

  if (!with_clip) {
    return;
  }
  out.push_back(StartRecording{id, policy_.clip});
  recording_ = RecordingState::Active;
  recording_event_ = ActiveEvent{id, kind};
}

void Core::start_call(const PeerId& peer, std::vector<Action>& out) {
  viewers_.insert(peer);
  out.push_back(StartCall{peer});
}

void Core::pump_alerts(TimePoint now, std::vector<Action>& out) {
  if (shutting_down_) {
    return;
  }
  alerts_.expire(now);
  const auto entry = alerts_.ready(server_online_, now);
  if (!entry) {
    return;
  }
  alerts_.mark_sent();
  out.push_back(SendAlert{entry->event_id, entry->kind, entry->triggered_at});
}

void Core::pump_uploads(TimePoint now, std::vector<Action>& out) {
  if (shutting_down_ || upload_in_flight_ || uploads_.empty() || !server_online_) {
    return;
  }
  if (upload_retry_after_ && now < *upload_retry_after_) {
    return;
  }
  upload_in_flight_ = true;
  out.push_back(UploadClip{uploads_.front().event_id, uploads_.front().path});
}

LedPattern Core::desired_led() const {
  if (shutting_down_) {
    return LedPattern::Off;
  }
  if (ring_led_until_) {
    return LedPattern::Ring;
  }
  if (!viewers_.empty()) {
    return LedPattern::Live;
  }
  if (recording_ != RecordingState::Idle) {
    return LedPattern::Recording;
  }
  if (!server_online_) {
    return LedPattern::Offline;
  }
  return LedPattern::Idle;
}

void Core::update_led(std::vector<Action>& out) {
  const LedPattern wanted = desired_led();
  if (wanted == led_) {
    return;
  }
  led_ = wanted;
  out.push_back(SetLed{wanted});
}

}  // namespace porch
