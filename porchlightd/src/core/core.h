#pragma once

#include <cstdint>
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
    // Kept because the clip has to report it long after the recording ends,
    // and by then the alert that carried it may already have been sent.
    TimePoint triggered_at;
    // Set when the core asks the recorder to stop before its time is up. The
    // fact is causal rather than measured: comparing the finished duration
    // against the requested one would only ever be a guess, since a clip
    // always comes back a little short of what was asked for.
    bool cut_short = false;
    // An alert being held back for the camera: a motion until it says whether
    // anybody is there, a ring only until it says who. Cleared when the alert
    // goes out; the deadline is when the core stops waiting and sends it anyway.
    bool awaiting_verdict = false;
    TimePoint verdict_due{};
    // The time the held alert will carry. The PIR's for a motion, the press's
    // for a ring - which, for a ring that took over a motion recording, is not
    // triggered_at: the clip still starts where the motion did.
    TimePoint alert_at{};
    // Who the camera saw, from a verdict that has already been acted on. A
    // press later in the same recording rings with the name the motion found.
    std::string faces{};
    // The camera said nobody. The recording is being stopped and the clip is
    // thrown away when it lands - unless the button is pressed first.
    bool unwanted = false;
  };

  // A clip on disk with everything needed to describe it, waiting its turn.
  // The size is kept beside it rather than inside it: the uploader measures the
  // file itself, and the only thing that needs this is the spool cap.
  struct PendingUpload {
    UploadClip clip;
    std::uintmax_t bytes = 0;
  };

  void on_button(TimePoint now, std::vector<Action>& out);
  void on_motion(TimePoint now, std::vector<Action>& out);
  void on_viewer(const ViewerRequested& event, std::vector<Action>& out);
  void on_call_ended(const CallEnded& event);
  void on_recording_finished(const RecordingFinished& event, TimePoint now,
                             std::vector<Action>& out);
  void on_detection(const DetectionResult& event, TimePoint now, std::vector<Action>& out);
  void on_upload_finished(const UploadFinished& event, TimePoint now);
  void on_server_offline(const ServerOffline& event, TimePoint now);
  void on_shutdown(std::vector<Action>& out);

  void trigger(Kind kind, TimePoint now, bool with_clip, std::vector<Action>& out);
  void watch_for_person(TimePoint now, std::vector<Action>& out);
  void ring_with_a_name(TimePoint now, std::vector<Action>& out);
  void release_held_alert(TimePoint now, std::vector<Action>& out,
                          const std::string& faces = {});
  void check_verdict_deadline(TimePoint now, std::vector<Action>& out);
  void start_call(const PeerId& peer, std::vector<Action>& out);
  void lapse(TimePoint now);
  void pump_alerts(TimePoint now, std::vector<Action>& out);
  void pump_uploads(TimePoint now, std::vector<Action>& out);
  void enforce_spool_cap(std::vector<Action>& out);
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
  // The link is not merely down, it is refusing us: a rejected credential, a
  // connection replaced by another of our role, or no credential at all. Only
  // the LED reads it - everything else about being offline is identical, and
  // making the queue behave differently would drop alerts on the one failure
  // where a person is already on their way to the device.
  bool server_fault_ = false;
  AlertQueue alerts_;

  std::deque<PendingUpload> uploads_;
  bool upload_in_flight_ = false;
  Backoff upload_backoff_;
  std::optional<TimePoint> upload_retry_after_;

  LedPattern led_ = LedPattern::Off;
  bool shutting_down_ = false;
};

}  // namespace porch
