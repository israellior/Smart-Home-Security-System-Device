#pragma once

#include <utility>

#include "event_sink.h"
#include "io/clip_uploader.h"
#include "iso8601.h"
#include "logging.h"

namespace porch {

class LoggingUploader : public ClipUploader {
 public:
  explicit LoggingUploader(EventSink sink) : sink_(std::move(sink)) {}

  // Prints the three steps the real one will take, so the sequence is visible
  // from the keyboard. ok reports the confirm, not the PUT. The confirm body is
  // printed in full because it is the part a server can get wrong quietly.
  void upload(const UploadClip& clip) override {
    log(Level::Info, "upload", "would POST /api/clips/{}/upload-url", clip.event_id);
    log(Level::Info, "upload", "would PUT {} to the url it returned", clip.path);
    log(Level::Info, "upload", "would POST /api/clips/{}/confirm kind={} at={} durationMs={}{}",
        clip.event_id, to_string(clip.kind), iso8601_of(clip.triggered_at),
        clip.duration.count(), clip.partial ? " partial=true" : "");
    sink_(UploadFinished{clip.event_id, true});
  }

  void discard(const EventId& event_id, const std::string& path) override {
    log(Level::Info, "upload", "would discard id={} path={}", event_id, path);
  }

 private:
  EventSink sink_;
};

}  // namespace porch
