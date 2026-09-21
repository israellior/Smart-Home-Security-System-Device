#include "io/fake_recorder.h"

#include <format>
#include <utility>

#include "logging.h"

namespace porch {
namespace {

// Roughly what 2000 kbit/s of video plus Opus would come to, so the core's
// "is this file plausible" check has something believable to judge.
std::uintmax_t plausible_bytes(std::chrono::milliseconds duration) {
  return static_cast<std::uintmax_t>(duration.count()) * 280;
}

}  // namespace

FakeRecorder::FakeRecorder(Reactor& reactor, EventSink sink, std::filesystem::path spool)
    : reactor_(reactor), sink_(std::move(sink)), spool_(std::move(spool)) {
  reactor_.watch(timer_.fd(), [this] {
    timer_.drain();
    finish();
  });
}

FakeRecorder::~FakeRecorder() { reactor_.unwatch(timer_.fd()); }

void FakeRecorder::start(const EventId& event_id, std::chrono::seconds seconds) {
  if (current_) {
    // The core never does this; if it ever did, losing the first clip silently
    // would be the worst possible answer.
    log(Level::Warn, "rec", "start for {} while {} is running; ending the first", event_id,
        *current_);
    finish();
  }
  current_ = event_id;
  started_at_ = Clock::now();
  timer_.arm_in(seconds);
  log(Level::Info, "rec", "would record id={} for {}s", event_id, seconds.count());
}

void FakeRecorder::stop() {
  if (!current_) {
    return;  // a stop with nothing running is not an error
  }
  timer_.disarm();
  finish();
}

void FakeRecorder::finish() {
  if (!current_) {
    return;  // the timer fired just after a stop already dealt with it
  }
  const auto duration =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started_at_);

  RecordingFinished finished;
  finished.event_id = *current_;
  finished.path = (spool_ / std::format("{}.mp4", *current_)).string();
  finished.ok = true;
  finished.duration = duration;
  finished.bytes = plausible_bytes(duration);

  log(Level::Info, "rec", "finished id={} duration={}ms", finished.event_id, duration.count());
  current_.reset();
  sink_(std::move(finished));
}

}  // namespace porch
