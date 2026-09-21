#pragma once

#include "core/actions.h"

namespace porch {

// Alerts out, and later viewer requests in. Every send must answer with an
// AlertResult event - Delivered, Rejected or Failed - or the core will wait
// for one forever. The link also reports ServerOnline and ServerOffline.
//
// Real backend later: a WebSocket to the app server. See docs/protocol.md,
// including why it must not sign in as role 'pi'.
class ServerLink {
 public:
  virtual ~ServerLink() = default;
  virtual void start() = 0;
  virtual void send_alert(const SendAlert& alert) = 0;
};

}  // namespace porch
