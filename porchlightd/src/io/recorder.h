#pragma once

#include <chrono>

#include "core/types.h"

namespace porch {

// Records one clip at a time.
//
// Two obligations, both of which the core depends on. It must always answer a
// start with exactly one RecordingFinished - ok=false if the recording died,
// so a crashed encoder is still an answer rather than silence. And a stop
// arriving immediately after a start must be safe: the clip ends early and
// reports the length it actually got, which the core then judges against
// min_clip.
//
// Real backend later: gst-launch-1.0 as a child process, finishing with EOS so
// the MP4 is playable, watched through a pidfd.
class Recorder {
 public:
  virtual ~Recorder() = default;

  virtual void start(const EventId& event_id, std::chrono::seconds seconds) = 0;
  virtual void stop() = 0;

  // Shutdown waits on this, so the clip is not left unplayable.
  virtual bool busy() const = 0;
};

}  // namespace porch
