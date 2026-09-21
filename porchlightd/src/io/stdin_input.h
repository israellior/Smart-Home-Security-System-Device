#pragma once

#include <functional>
#include <string>
#include <string_view>

#include "event_sink.h"
#include "io/input_source.h"
#include "reactor.h"

namespace porch {

// The whole doorbell, driven from the keyboard. Type "button" and every rule
// fires exactly as it would with a real switch.
//
// It also owns the two commands no real input has - "online" and "offline" -
// because the queue, the ageing and the backoff are otherwise unreachable
// without unplugging something.
class StdinInput : public InputSource {
 public:
  using LinkControl = std::function<void(bool online)>;

  StdinInput(Reactor& reactor, EventSink sink, LinkControl link);
  ~StdinInput() override;

  void start() override;

 private:
  void on_readable();
  void handle_line(std::string_view line);
  void print_help() const;

  Reactor& reactor_;
  EventSink sink_;
  LinkControl link_;
  std::string buffer_;
  std::string last_peer_{"viewer-1"};
  bool watching_ = false;
};

}  // namespace porch
