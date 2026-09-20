#pragma once

#include <chrono>
#include <cstddef>

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

  std::chrono::seconds alert_max_age{900};
  std::chrono::seconds alert_backoff_initial{2};
  std::chrono::seconds alert_backoff_max{60};
  std::size_t max_queued_alerts{32};
};

}  // namespace porch
