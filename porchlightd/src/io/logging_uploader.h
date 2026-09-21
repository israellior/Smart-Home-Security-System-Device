#pragma once

#include <utility>

#include "event_sink.h"
#include "io/clip_uploader.h"
#include "logging.h"

namespace porch {

class LoggingUploader : public ClipUploader {
 public:
  explicit LoggingUploader(EventSink sink) : sink_(std::move(sink)) {}

  void upload(const EventId& event_id, const std::string& path) override {
    log(Level::Info, "upload", "would upload id={} path={}", event_id, path);
    sink_(UploadFinished{event_id, true});
  }

  void discard(const EventId& event_id, const std::string& path) override {
    log(Level::Info, "upload", "would discard id={} path={}", event_id, path);
  }

 private:
  EventSink sink_;
};

}  // namespace porch
