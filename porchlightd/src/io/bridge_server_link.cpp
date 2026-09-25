#include "io/bridge_server_link.h"

#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <nlohmann/json.hpp>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

#include "core/clock.h"
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
      device_id_(std::move(device_id)),
      restart_backoff_(config_.restart_backoff_initial, config_.restart_backoff_max) {
  reactor_.watch(ack_timer_.fd(), [this] {
    ack_timer_.drain();
    on_ack_timeout();
  });
  reactor_.watch(restart_timer_.fd(), [this] {
    restart_timer_.drain();
    try_spawn();
  });
}

BridgeServerLink::~BridgeServerLink() {
  if (bridge_.pid > 0) {
    ::kill(bridge_.pid, SIGTERM);
    int status = 0;
    ::waitpid(bridge_.pid, &status, 0);
  }
  release();
  reactor_.unwatch(restart_timer_.fd());
  reactor_.unwatch(ack_timer_.fd());
}

void BridgeServerLink::start() { try_spawn(); }

// The credential is the one file the daemon needs and cannot make for itself,
// and the way it usually goes wrong is not that it is absent but that it is
// 0600 and owned by somebody else: the unit runs as `porchlight`, and a
// credential written by hand belongs to whoever typed it.
//
// That matters because of where it surfaces otherwise. The bridge would exit,
// the server would never see a hello, and the only evidence would be an
// authentication failure - which reads as "this device must be re-minted" when
// the fix is one chmod. Checking here costs an open(), and only on an attempt
// that is about to be made anyway.
bool BridgeServerLink::credential_readable() const {
  std::error_code ec;
  if (!std::filesystem::exists(config_.credential_path, ec)) {
    log(Level::Error, "server", "there is no credential at {}. The device is not provisioned: "
        "mint one on the server and write it there.", config_.credential_path.string());
    return false;
  }
  const std::ifstream file(config_.credential_path);
  if (!file) {
    log(Level::Error, "server", "cannot read {}: {}. Check the owner and mode - this runs as "
        "the porchlight user and the file is meant to be 0600.",
        config_.credential_path.string(), std::strerror(errno));
    return false;
  }
  return true;
}

void BridgeServerLink::try_spawn() {
  if (given_up_) {
    return;
  }
  if (!credential_readable()) {
    // Permanent to the LED, because no amount of waiting writes a credential -
    // somebody has to. Not permanent to this object, though: the wait goes on,
    // so writing the file while the daemon is running brings the link up within
    // a backoff instead of needing a restart as well. That is exactly what
    // happens during provisioning.
    report_down(true, "no usable credential");
    schedule_restart();
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
    // Permanent for the same reason a missing credential is: the bridge is not
    // going to install itself, and a device whose light says "offline" is a
    // device somebody waits for. The retry continues regardless, so putting
    // the file there while the daemon runs is enough.
    report_down(true, "the bridge will not start");
    schedule_restart();
    return;
  }
  reactor_.watch(bridge_.from_child.get(), [this] { on_readable(); });
  log(Level::Info, "server", "bridge started, connecting to {}", config_.base_url);
}

void BridgeServerLink::schedule_restart() {
  if (given_up_) {
    return;
  }
  // Both ends of the subtraction are the same `now`, deliberately. Asking the
  // clock twice loses the fraction of a millisecond in between, which truncates
  // a one-second wait to zero - and arming a timerfd for zero does not fire it,
  // it disarms it. The bridge would then never be started again at all.
  const TimePoint now = Clock::now();
  const auto wait = std::chrono::duration_cast<std::chrono::seconds>(restart_backoff_.fail(now) - now);
  log(Level::Info, "server", "starting the bridge again in {}s", wait.count());
  restart_timer_.arm_in(wait);
}

// Said to the core once per outage rather than once per attempt. `permanent`
// is what separates "the network will be back" from "somebody has to come and
// fix this", and only the LED reads the difference. Both are reported once: a
// credential that is still missing on the ninth attempt is not news, and a log
// line every two seconds is how a device with one broken thing looks like a
// device with an unstable network.
void BridgeServerLink::report_down(bool permanent, const std::string& why) {
  if (down_reported_ && (!permanent || fault_reported_)) {
    return;
  }
  down_reported_ = true;
  fault_reported_ = fault_reported_ || permanent;
  online_ = false;
  log(Level::Warn, "server", "link down: {}", why);
  sink_(ServerOffline{permanent});
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
    down_reported_ = false;
    fault_reported_ = false;
    // A connection that actually worked is the only evidence that whatever was
    // wrong has stopped being wrong, so it is the only thing that resets the
    // wait. The bridge resets its own reconnect delay on the same event.
    restart_backoff_.reset();
    restart_timer_.disarm();
    log(Level::Info, "server", "connected");
    sink_(ServerOnline{});
  } else if (type == "offline") {
    log(Level::Warn, "server", "disconnected: {}", message.value("reason", "?"));
    report_down(false, message.value("reason", "?"));
  } else if (type == "fatal") {
    const std::string reason = message.value("reason", "?");
    log(Level::Error, "server", "the link cannot recover: {}. Nothing here will retry - "
        "the device needs a person.", reason);
    given_up_ = true;
    restart_timer_.disarm();
    report_down(true, reason);
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

  report_down(false, why);
  if (awaiting_ack_) {
    awaiting_ack_ = false;
    sink_(AlertResult{pending_id_, pending_kind_, AlertOutcome::Failed});
  }
  // The bridge reconnects by itself, so it only exits when it has given up or
  // been killed. Starting it again is the right answer for the second case and
  // given_up_ covers the first - but after a wait, because a bridge that dies
  // on exec dies on the next exec just as fast.
  schedule_restart();
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
