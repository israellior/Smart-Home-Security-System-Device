#pragma once

#include <filesystem>
#include <optional>
#include <string>

#include "core/clock.h"
#include "event_sink.h"
#include "io/recorder.h"
#include "reactor.h"
#include "timer_fd.h"

namespace porch {

// Records nothing, but takes the right amount of time doing it, because the
// rules that matter are about timing: a viewer arriving mid-clip, a clip too
// short to keep, a shutdown waiting for one to finish.
//
// It owns its own TimerFd rather than borrowing the reactor's, which belongs
// to the core's deadlines. The real recorder will watch a pidfd the same way.
class FakeRecorder : public Recorder {
 public:
  FakeRecorder(Reactor& reactor, EventSink sink, std::filesystem::path spool);
  ~FakeRecorder() override;

  void start(const EventId& event_id, std::chrono::seconds seconds) override;
  void stop() override;
  bool busy() const override { return current_.has_value(); }

 private:
  void finish();

  Reactor& reactor_;
  EventSink sink_;
  std::filesystem::path spool_;
  TimerFd timer_;
  std::optional<EventId> current_;
  TimePoint started_at_;
};

}  // namespace porch
