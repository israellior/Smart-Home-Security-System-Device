#pragma once

#include <deque>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/actions.h"
#include "core/alert_queue.h"
#include "core/backoff.h"
#include "core/clock.h"
#include "core/events.h"
#include "core/ids.h"
#include "core/policy.h"
#include "core/types.h"

namespace porch {

// Every rule the device obeys, and no I/O at all. An event and a time go in, a
// list of actions comes out, and the same input always gives the same output.
class Core {
 public:
  Core(Policy policy, IdSource& ids);

  std::vector<Action> handle(const Event& event, TimePoint now);

  // When the loop should send a Tick. Nothing pending means no timer at all.
  std::optional<TimePoint> next_deadline() const;

 private:
  // Stopping is not a nicety: EOS has to flush through the muxer before the
  // recorder's process releases the camera and the sound card, and the call
  // cannot start until it has.
  enum class RecordingState { Idle, Active, Stopping };

  struct ActiveEvent {
    EventId id;
    Kind kind = Kind::Motion;
  };

  struct PendingUpload {
    EventId event_id;
    std::string path;
  };

  void on_button(TimePoint now, std::vector<Action>& out);
  void on_motion(TimePoint now, std::vector<Action>& out);
  void on_viewer(const ViewerRequested& event, std::vector<Action>& out);
  void on_call_ended(const CallEnded& event);
  void on_recording_finished(const RecordingFinished& event, std::vector<Action>& out);
  void on_upload_finished(const UploadFinished& event, TimePoint now);
  void on_server_offline(TimePoint now);
  void on_shutdown(std::vector<Action>& out);

  void trigger(Kind kind, TimePoint now, bool with_clip, std::vector<Action>& out);
  void start_call(const PeerId& peer, std::vector<Action>& out);
  void lapse(TimePoint now);
  void pump_alerts(TimePoint now, std::vector<Action>& out);
  void pump_uploads(TimePoint now, std::vector<Action>& out);
  void update_led(std::vector<Action>& out);
  LedPattern desired_led() const;

  Policy policy_;
  IdSource& ids_;

  std::set<PeerId> viewers_;
  RecordingState recording_ = RecordingState::Idle;
  std::optional<ActiveEvent> recording_event_;
  std::optional<PeerId> deferred_call_;

  std::optional<TimePoint> motion_cooldown_until_;
  std::optional<TimePoint> press_window_until_;
  std::optional<TimePoint> ring_led_until_;

  bool server_online_ = false;
  AlertQueue alerts_;

  std::deque<PendingUpload> uploads_;
  bool upload_in_flight_ = false;
  Backoff upload_backoff_;
  std::optional<TimePoint> upload_retry_after_;

  LedPattern led_ = LedPattern::Off;
  bool shutting_down_ = false;
};

}  // namespace porch
