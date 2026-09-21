#pragma once

#include <sys/types.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

#include "config.h"
#include "core/clock.h"
#include "event_sink.h"
#include "io/recorder.h"
#include "reactor.h"
#include "timer_fd.h"
#include "unique_fd.h"

namespace porch {

// Turning configuration into a command line is the one part of this that can
// be checked without a camera or a sound card, so it is reachable on its own.
std::vector<std::string> build_pipeline(const RecorderConfig& config,
                                        const std::filesystem::path& output);

// gst-launch-1.0 as a child process, watched through a pidfd.
//
// A separate process because a codec that segfaults should cost a clip, not
// the doorbell, and because GStreamer would otherwise bring its own threads
// and main loop into a daemon that has neither.
class GstRecorder : public Recorder {
 public:
  GstRecorder(Reactor& reactor, EventSink sink, RecorderConfig config,
              std::filesystem::path spool);
  ~GstRecorder() override;

  void start(const EventId& event_id, std::chrono::seconds seconds) override;
  void stop() override;
  bool busy() const override { return phase_ != Phase::Idle; }

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
};

}  // namespace porch
