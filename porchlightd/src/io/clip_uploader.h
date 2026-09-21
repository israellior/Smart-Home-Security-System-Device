#pragma once

#include <string>

#include "core/types.h"

namespace porch {

// Owns what happens to a finished clip. It deletes after a successful upload,
// which is why discarding one is its job too rather than a separate concern.
//
// Real backend later: an HTTPS PUT carrying the device credential and the
// sidecar metadata. See docs/protocol.md.
class ClipUploader {
 public:
  virtual ~ClipUploader() = default;

  // Must answer with an UploadFinished event, or the clip is never retried.
  virtual void upload(const EventId& event_id, const std::string& path) = 0;

  // Nothing is coming back for this one: the clip is simply gone.
  virtual void discard(const EventId& event_id, const std::string& path) = 0;
};

}  // namespace porch
