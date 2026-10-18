#pragma once

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "config.h"
#include "core/clock.h"
#include "core/types.h"
#include "event_sink.h"
#include "reactor.h"
#include "timer_fd.h"
#include "unique_fd.h"

namespace porch {

// Turning configuration into a command line is the one part of this that can
// be checked without a camera or a sound card, so it is reachable on its own.
// detect_for above zero adds the detector's branch and its switches.
std::vector<std::string> build_pipeline(const RecorderConfig& config,
                                        const std::filesystem::path& output,
                                        std::chrono::seconds detect_for = std::chrono::seconds{0});

// pi/record-clip.py - gst-launch-1.0 -e with the clock pinned - as a child
// process, watched through a pidfd.
//
// A separate process because a codec that segfaults should cost a clip, not
// the doorbell, and because GStreamer would otherwise bring its own threads
// and main loop into a daemon that has neither.
//
// Two obligations, both of which the core depends on. Every start is answered
// with exactly one RecordingFinished - ok=false if the recording died, so a
// crashed encoder is still an answer rather than silence. And a stop arriving
// immediately after a start is safe: the clip ends early and reports the
// length it actually got, which the core then judges against min_clip.
class GstRecorder {
 public:
  GstRecorder(Reactor& reactor, EventSink sink, RecorderConfig config,
              std::filesystem::path spool);
  ~GstRecorder();

  // detect_for above zero also runs the person detector on the same frames,
  // which answers with at most one DetectionResult, always before the
  // RecordingFinished. Zero means no detector and no DetectionResult.
  void start(const EventId& event_id, std::chrono::seconds seconds,
             std::chrono::seconds detect_for);
  void stop();

  // Shutdown waits on this, so the clip is not left unplayable.
  bool busy() const { return phase_ != Phase::Idle; }

 private:
  // Stopping is the EOS flush. It is not instant: the muxer has to write the
  // moov atom before the process exits, and until it does the camera and the
  // sound card are still held.
  enum class Phase { Idle, Recording, Stopping };

  void on_timer();
  void on_child_exit();
  void begin_stop();
  void report(bool clean_exit);
  void release_child();
  void read_detector();
  void handle_detector_line(const std::string& line);

  Reactor& reactor_;
  EventSink sink_;
  RecorderConfig config_;
  std::filesystem::path spool_;
  TimerFd timer_;

  Phase phase_ = Phase::Idle;
  EventId event_id_;
  std::filesystem::path output_;
  TimePoint started_at_;
  pid_t pid_ = -1;
  UniqueFd pidfd_;
  bool killed_ = false;

  // The child's stdout, when it detects: one JSON line carrying the verdict,
  // and nothing else. Its stderr stays the journal's.
  UniqueFd detector_;
  std::string detector_input_;
  bool verdict_sent_ = false;
};

}  // namespace porch
