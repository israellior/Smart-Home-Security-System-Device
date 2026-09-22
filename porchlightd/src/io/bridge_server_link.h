#pragma once

#include <string>

#include "child_process.h"
#include "config.h"
#include "event_sink.h"
#include "io/server_link.h"
#include "reactor.h"
#include "timer_fd.h"

namespace porch {

// The app server, reached through pi/server-bridge.py.
//
// C++ has no WebSocket and this Pi already runs python3-websocket for the
// media script, so the socket lives in a child process and this exchanges
// JSON lines with it. The bridge holds the connection and reconnects; all the
// policy - what to retry, what to drop, how long to wait - stays in the core.
//
// The one translation that matters is silence. The server signals a transient
// failure by not acknowledging at all, so an alert with no reply inside
// ack_timeout becomes AlertOutcome::Failed and the core retries it. ok:false
// becomes Rejected and is never retried.
class BridgeServerLink : public ServerLink {
 public:
  BridgeServerLink(Reactor& reactor, EventSink sink, ServerConfig config,
                   std::string device_id);
  ~BridgeServerLink() override;

  void start() override;
  void send_alert(const SendAlert& alert) override;

 private:
  void spawn();
  void on_readable();
  void handle_line(const std::string& line);
  void on_ack_timeout();
  void on_bridge_lost(const char* why);
  void release();

  Reactor& reactor_;
  EventSink sink_;
  ServerConfig config_;
  std::string device_id_;
  TimerFd ack_timer_;

  PipedChild bridge_;
  std::string incoming_;
  bool online_ = false;
  // Set when the bridge reports something no restart can fix - a rejected
  // credential, or another connection of our role taking over.
  bool given_up_ = false;

  // What we are waiting for an acknowledgement about. The core sends one
  // alert at a time, so one slot is enough.
  bool awaiting_ack_ = false;
  EventId pending_id_;
  Kind pending_kind_ = Kind::Motion;
};

}  // namespace porch
