#pragma once

#include <utility>

#include "event_sink.h"
#include "io/clip_uploader.h"
#include "logging.h"

namespace porch {

class LoggingUploader : public ClipUploader {
 public:
  explicit LoggingUploader(EventSink sink) : sink_(std::move(sink)) {}

  // Prints the three steps the real one will take, so the sequence is visible
  // from the keyboard. ok reports the confirm, not the PUT.
  void upload(const EventId& event_id, const std::string& path) override {
    log(Level::Info, "upload", "would POST /api/clips/{}/upload-url", event_id);
    log(Level::Info, "upload", "would PUT {} to the url it returned", path);
    log(Level::Info, "upload", "would POST /api/clips/{}/confirm", event_id);
    sink_(UploadFinished{event_id, true});
  }

  void discard(const EventId& event_id, const std::string& path) override {
    log(Level::Info, "upload", "would discard id={} path={}", event_id, path);
  }

 private:
  EventSink sink_;
};

}  // namespace porch
