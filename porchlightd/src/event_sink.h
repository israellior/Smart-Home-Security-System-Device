#pragma once

#include <functional>

#include "core/events.h"

namespace porch {

// How a backend reports something back. It queues the event rather than
// handling it, so a backend can never re-enter the core from inside an action
// it is still carrying out.
using EventSink = std::function<void(Event)>;

}  // namespace porch
