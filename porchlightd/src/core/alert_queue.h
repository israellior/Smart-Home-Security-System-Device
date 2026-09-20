#pragma once

#include <cstddef>
#include <deque>
#include <optional>

#include "core/backoff.h"
#include "core/clock.h"
#include "core/policy.h"
#include "core/types.h"

namespace porch {

// Alerts waiting for the server, in order. Bounded, aged and backed off, all of
// which are rules the core is tested on rather than details of a transport.
class AlertQueue {
 public:
  struct Entry {
    EventId event_id;
    Kind kind = Kind::Motion;
    TimePoint triggered_at;
  };

  explicit AlertQueue(const Policy& policy);

  // At the limit the oldest is dropped: after a long outage the most recent
  // events are the ones worth delivering.
  void push(Entry entry);

  // Drops alerts too old to be worth sending. The one in flight is left alone.
  void expire(TimePoint now);

  std::optional<Entry> ready(bool online, TimePoint now) const;
  void mark_sent();
  void on_result(const EventId& event_id, Kind kind, AlertOutcome outcome, TimePoint now);

  // A dropped link means whatever was in flight never landed.
  void link_lost(TimePoint now);

  std::optional<TimePoint> next_deadline() const;

  std::size_t size() const { return entries_.size(); }
  bool empty() const { return entries_.empty(); }

 private:
  std::chrono::seconds max_age_;
  std::size_t capacity_;
  std::deque<Entry> entries_;
  bool in_flight_ = false;
  Backoff backoff_;
  std::optional<TimePoint> retry_after_;
};

}  // namespace porch
