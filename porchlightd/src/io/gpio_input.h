#pragma once

#include <cstdint>

#include "config.h"
#include "event_sink.h"
#include "io/input_source.h"
#include "reactor.h"

// libgpiod's own types, forward-declared so that <gpiod.h> stays inside the
// .cpp and backends.cpp does not acquire the dependency by including this.
struct gpiod_line_request;
struct gpiod_edge_event_buffer;

namespace porch {

// The button and the PIR, on one libgpiod v2 request and so on one descriptor:
// the kernel queues edges from both lines into the same fd and stamps each one
// with the line it came from, which is why this watches a single thing.
//
// Contact debounce is the kernel's, asked for per line through the request -
// see the note in the .cpp for why there is no debounce of ours.
class GpioInput : public InputSource {
 public:
  GpioInput(Reactor& reactor, EventSink sink, const GpioConfig& config);
  ~GpioInput() override;

  void start() override;

 private:
  void on_readable();
  void handle(unsigned int line, bool asserted, std::uint64_t at_ns);

  Reactor& reactor_;
  EventSink sink_;
  gpiod_line_request* request_ = nullptr;
  gpiod_edge_event_buffer* events_ = nullptr;
  unsigned int button_ = 0;
  unsigned int motion_ = 0;
  // When each line last went active, so the release can report how long it was
  // held. The PIR's pulse width is the one number that tells an AM312 from an
  // HC-SR501 with its delay pot wound up, and it is otherwise unobservable.
  std::uint64_t button_since_ns_ = 0;
  std::uint64_t motion_since_ns_ = 0;
  bool watching_ = false;
};

}  // namespace porch
