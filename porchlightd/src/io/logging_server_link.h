#pragma once

#include <string>
#include <utility>

#include "event_sink.h"
#include "io/server_link.h"
#include "logging.h"

namespace porch {

// Prints the alert it would have sent and answers Delivered at once. Going
// offline is driven from the fake input, which is how the queue, the ageing
// and the backoff can be exercised by hand.
class LoggingServerLink : public ServerLink {
 public:
  LoggingServerLink(std::string device_id, EventSink sink)
      : device_id_(std::move(device_id)), sink_(std::move(sink)) {}

  void start() override {
    log(Level::Info, "server", "connected device={}", device_id_);
    sink_(ServerOnline{});
  }

  void send_alert(const SendAlert& alert) override {
    if (!online_) {
      // The core should not have asked, so this says so rather than hiding it.
      log(Level::Warn, "server", "asked to send {} while offline", alert.event_id);
      sink_(AlertResult{alert.event_id, alert.kind, AlertOutcome::Failed});
      return;
    }
    log(Level::Info, "server", "would send event id={} kind={} device={}", alert.event_id,
        to_string(alert.kind), device_id_);
    sink_(AlertResult{alert.event_id, alert.kind, AlertOutcome::Delivered});
  }

  // Called by the fake input, not by the core.
  void set_online(bool online) {
    if (online == online_) {
      return;
    }
    online_ = online;
    log(Level::Info, "server", "link {}", online ? "up" : "down");
    if (online) {
      sink_(ServerOnline{});
    } else {
      sink_(ServerOffline{});
    }
  }

 private:
  std::string device_id_;
  EventSink sink_;
  bool online_ = true;
};

}  // namespace porch
