#include "io/bridge_server_link.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <utility>

#include "iso8601.h"
#include "logging.h"

namespace porch {
namespace {

using nlohmann::json;

}  // namespace

BridgeServerLink::BridgeServerLink(Reactor& reactor, EventSink sink, ServerConfig config,
                                   std::string device_id)
    : reactor_(reactor),
      sink_(std::move(sink)),
      config_(std::move(config)),
      device_id_(std::move(device_id)) {
  reactor_.watch(ack_timer_.fd(), [this] {
    ack_timer_.drain();
    on_ack_timeout();
  });
}

BridgeServerLink::~BridgeServerLink() {
  if (bridge_.pid > 0) {
    ::kill(bridge_.pid, SIGTERM);
    int status = 0;
    ::waitpid(bridge_.pid, &status, 0);
  }
  release();
  reactor_.unwatch(ack_timer_.fd());
}

void BridgeServerLink::start() { spawn(); }

void BridgeServerLink::spawn() {
  if (given_up_) {
    return;
  }
  bridge_ = spawn_child_with_pipes({
      config_.bridge_path.string(),
      "--url", config_.base_url,
      "--device-id", device_id_,
      "--credential-file", config_.credential_path.string(),
  });
  if (bridge_.pid < 0) {
    log(Level::Error, "server", "cannot start {}: {}", config_.bridge_path.string(),
        std::strerror(errno));
    return;
  }
  reactor_.watch(bridge_.from_child.get(), [this] { on_readable(); });
  log(Level::Info, "server", "bridge started, connecting to {}", config_.base_url);
}

void BridgeServerLink::send_alert(const SendAlert& alert) {
  if (bridge_.pid < 0 || !bridge_.to_child.valid()) {
    // No bridge, so no acknowledgement will ever come. Say so at once rather
    // than making the core wait out the timeout for something that cannot work.
    sink_(AlertResult{alert.event_id, alert.kind, AlertOutcome::Failed});
    return;
  }

  const json frame{
      {"type", "event"},
      {"eventId", alert.event_id},
      {"kind", to_string(alert.kind)},
      {"at", iso8601_of(alert.triggered_at)},
  };
  const std::string line = frame.dump() + "\n";

  const ssize_t written = ::write(bridge_.to_child.get(), line.data(), line.size());
  if (written != static_cast<ssize_t>(line.size())) {
    // The write end is non-blocking on purpose: a wedged bridge must not stall
    // the loop. A partial or refused write is just another silent failure.
    log(Level::Warn, "server", "could not hand {} to the bridge: {}", alert.event_id,
        written < 0 ? std::strerror(errno) : "short write");
    sink_(AlertResult{alert.event_id, alert.kind, AlertOutcome::Failed});
    return;
  }

  awaiting_ack_ = true;
  pending_id_ = alert.event_id;
  pending_kind_ = alert.kind;
  ack_timer_.arm_in(config_.ack_timeout);
}

void BridgeServerLink::on_readable() {
  char chunk[1024];
  const ssize_t got = ::read(bridge_.from_child.get(), chunk, sizeof(chunk));
  if (got == 0) {
    on_bridge_lost("bridge closed its output");
    return;
  }
  if (got < 0) {
    if (errno != EAGAIN && errno != EINTR) {
      on_bridge_lost(std::strerror(errno));
    }
    return;
  }

  incoming_.append(chunk, static_cast<std::size_t>(got));
  std::size_t newline = incoming_.find('\n');
  while (newline != std::string::npos) {
    handle_line(incoming_.substr(0, newline));
    incoming_.erase(0, newline + 1);
    newline = incoming_.find('\n');
  }
}

void BridgeServerLink::handle_line(const std::string& line) {
  if (line.empty()) {
    return;
  }
  json message;
  try {
    message = json::parse(line);
  } catch (const json::parse_error&) {
    log(Level::Warn, "server", "bridge said something unparseable: {}", line.substr(0, 120));
    return;
  }

  const std::string type = message.value("type", "");
  if (type == "online") {
    online_ = true;
    log(Level::Info, "server", "connected");
    sink_(ServerOnline{});
  } else if (type == "offline") {
    online_ = false;
    log(Level::Warn, "server", "disconnected: {}", message.value("reason", "?"));
    sink_(ServerOffline{});
  } else if (type == "fatal") {
    const std::string reason = message.value("reason", "?");
    log(Level::Error, "server", "the link cannot recover: {}", reason);
    given_up_ = true;
    online_ = false;
    sink_(ServerOffline{});
  } else if (type == "event-ack") {
    const EventId id = message.value("eventId", "");
    const bool ok = message.value("ok", false);
    const Kind kind = message.value("kind", "") == "ring" ? Kind::Ring : Kind::Motion;
    if (!ok) {
      // Only ever returned for something that will be just as wrong next time.
      log(Level::Warn, "server", "{} rejected: {}", id,
          message.value("error", "no reason given"));
    }
    ack_timer_.disarm();
    awaiting_ack_ = false;
    sink_(AlertResult{id, kind, ok ? AlertOutcome::Delivered : AlertOutcome::Rejected});
  } else if (type == "viewer-requested") {
    const json peer = message.value("peer", json{});
    sink_(ViewerRequested{peer.is_string() ? peer.get<std::string>() : peer.dump()});
  } else {
    log(Level::Debug, "server", "ignoring {} from the bridge", type);
  }
}

void BridgeServerLink::on_ack_timeout() {
  if (!awaiting_ack_) {
    return;
  }
  // Silence is how the server signals a transient failure, so this is the
  // ordinary case rather than an error. The core backs off and sends it again.
  log(Level::Info, "server", "no acknowledgement for {}; it will be sent again", pending_id_);
  awaiting_ack_ = false;
  sink_(AlertResult{pending_id_, pending_kind_, AlertOutcome::Failed});
}

void BridgeServerLink::on_bridge_lost(const char* why) {
  log(Level::Warn, "server", "bridge ended: {}", why);
  if (bridge_.pid > 0) {
    int status = 0;
    ::waitpid(bridge_.pid, &status, 0);
  }
  release();

  if (online_) {
    online_ = false;
    sink_(ServerOffline{});
  }
  if (awaiting_ack_) {
    awaiting_ack_ = false;
    sink_(AlertResult{pending_id_, pending_kind_, AlertOutcome::Failed});
  }
  // The bridge reconnects by itself, so it only exits when it has given up or
  // been killed. Starting it again is the right answer for the second case and
  // given_up_ covers the first.
  spawn();
}

void BridgeServerLink::release() {
  if (bridge_.from_child.valid()) {
    reactor_.unwatch(bridge_.from_child.get());
  }
  bridge_.from_child.reset();
  bridge_.to_child.reset();
  bridge_.pid = -1;
  incoming_.clear();
}

}  // namespace porch
