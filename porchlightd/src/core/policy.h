#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>

namespace porch {

// Every timing rule the core obeys. Held apart from the rest of the
// configuration so tests can build one by hand without reading a file.
struct Policy {
  std::chrono::seconds clip{15};
  // A clip shorter than this is discarded rather than uploaded: a viewer who
  // arrives a moment after a trigger cuts the recording to nearly nothing.
  std::chrono::seconds min_clip{2};

  // Explicit rather than derived from clip, so changing the clip length does
  // not silently move the cooldown as well.
  std::chrono::seconds motion_cooldown{25};

  // Seconds, and a policy: repeated presses in this window share one alert.
  // Contact debounce is a different thing entirely and lives in InputSource.
  std::chrono::seconds press_dedup_window{5};

  std::chrono::seconds ring_led{5};

  // A day, and two hundred. The server signals a transient failure by not
  // acknowledging at all and expects the device to keep trying, so these are
  // deliberately far larger than a plausible outage. Tight values here mean
  // real doorbell presses are dropped before they are ever sent - a backlog
  // of forty against the live server needed three retry rounds to land.
  std::chrono::seconds alert_max_age{86400};
  std::size_t max_queued_alerts{200};

  // Shared by the alert queue and the clip uploader: both are waiting on the
  // same server, so they retry on the same schedule.
  std::chrono::seconds retry_backoff_initial{2};
  std::chrono::seconds retry_backoff_max{60};

  // What the clips waiting to go up may take on the card. There is no
  // equivalent of alert_max_age here: an alert costs nothing to keep and a
  // clip costs megabytes, so the limit is size and the oldest go first.
  std::uintmax_t spool_max_bytes{512ull * 1024 * 1024};
};

}  // namespace porch
