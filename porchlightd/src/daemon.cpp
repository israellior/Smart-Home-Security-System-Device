#include "daemon.h"

#include <utility>

#include "logging.h"

namespace porch {
namespace {

template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

}  // namespace

Daemon::Daemon(const Config& config, Reactor& reactor)
    : reactor_(reactor), core_(config.policy, ids_) {
  hardware_ = make_hardware(config, reactor_, [this](Event event) { deliver(std::move(event)); });

  reactor_.on_wake([this] { deliver(Tick{}); });
  reactor_.on_signal([this] {
    log(Level::Info, "main", "signal received, shutting down");
    deliver(Shutdown{});
  });
}

void Daemon::run() {
  hardware_.server->start();
  hardware_.input->start();
  reactor_.run();
}

// Every event enters here, whichever backend produced it.
void Daemon::deliver(Event event) {
  pending_.push_back(std::move(event));
  drain();
}

void Daemon::drain() {
  if (draining_) {
    return;  // a backend reported from inside an action; the loop below reaches it
  }
  draining_ = true;

  while (!pending_.empty()) {
    const Event event = std::move(pending_.front());
    pending_.pop_front();

    if (std::holds_alternative<Shutdown>(event)) {
      stopping_ = true;
    }
    for (const Action& action : core_.handle(event, Clock::now())) {
      execute(action);
    }
  }

  draining_ = false;

  // Shutdown waits for the recorder, so a clip in progress still finishes with
  // EOS and stays playable.
  if (stopping_ && !hardware_.recorder->busy()) {
    reactor_.stop();
    return;
  }
  reactor_.wake_at(core_.next_deadline());
}

void Daemon::execute(const Action& action) {
  // No generic fallback: a new action must fail to compile here rather than be
  // quietly dropped.
  std::visit(overloaded{
                 [&](const SetLed& a) { hardware_.led->show(a.pattern); },
                 [&](const PlayChime&) { hardware_.chime->play(); },
                 [&](const SendAlert& a) { hardware_.server->send_alert(a); },
                 [&](const StartRecording& a) { hardware_.recorder->start(a.event_id, a.seconds); },
                 [&](const StopRecording&) { hardware_.recorder->stop(); },
                 [&](const UploadClip& a) { hardware_.uploader->upload(a.event_id, a.path); },
                 [&](const DiscardClip& a) { hardware_.uploader->discard(a.event_id, a.path); },
                 [&](const StartCall& a) { hardware_.media->start_call(a.peer); },
                 [&](const StopCall&) { hardware_.media->stop_call(); },
             },
             action);
}

}  // namespace porch
