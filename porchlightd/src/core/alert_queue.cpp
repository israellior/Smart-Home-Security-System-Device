#include "core/alert_queue.h"

#include <algorithm>

namespace porch {

AlertQueue::AlertQueue(const Policy& policy)
    : max_age_(policy.alert_max_age),
      capacity_(policy.max_queued_alerts),
      backoff_(policy.retry_backoff_initial, policy.retry_backoff_max) {}

void AlertQueue::push(Entry entry) {
  if (entries_.size() >= capacity_) {
    // The front may be in flight, and dropping it would strand its result.
    const auto oldest_droppable = entries_.begin() + (in_flight_ ? 1 : 0);
    if (oldest_droppable != entries_.end()) {
      entries_.erase(oldest_droppable);
    }
  }
  entries_.push_back(std::move(entry));
}

void AlertQueue::expire(TimePoint now) {
  auto first = entries_.begin();
  if (in_flight_ && first != entries_.end()) {
    ++first;
  }
  const auto too_old = [&](const Entry& entry) { return now - entry.triggered_at >= max_age_; };
  entries_.erase(std::remove_if(first, entries_.end(), too_old), entries_.end());
}

std::optional<AlertQueue::Entry> AlertQueue::ready(bool online, TimePoint now) const {
  if (in_flight_ || !online || entries_.empty()) {
    return std::nullopt;
  }
  if (retry_after_ && now < *retry_after_) {
    return std::nullopt;
  }
  return entries_.front();
}

void AlertQueue::mark_sent() { in_flight_ = true; }

void AlertQueue::on_result(const EventId& event_id, Kind kind, AlertOutcome outcome,
                           TimePoint now) {
  if (!in_flight_ || entries_.empty()) {
    return;
  }
  const Entry& front = entries_.front();
  if (front.event_id != event_id || front.kind != kind) {
    return;  // a result for an alert that is no longer the one in flight
  }

  in_flight_ = false;
  if (outcome == AlertOutcome::Failed) {
    retry_after_ = backoff_.fail(now);
    return;
  }
  // Delivered and Rejected both finish the alert. A rejection is the server
  // refusing it, and retrying would be refused identically.
  entries_.pop_front();
  backoff_.reset();
  retry_after_.reset();
}

void AlertQueue::link_lost(TimePoint now) {
  if (!in_flight_) {
    return;
  }
  in_flight_ = false;
  retry_after_ = backoff_.fail(now);
}

std::optional<TimePoint> AlertQueue::next_deadline() const {
  std::optional<TimePoint> deadline = retry_after_;
  if (!entries_.empty()) {
    const TimePoint expires_at = entries_.front().triggered_at + max_age_;
    if (!deadline || expires_at < *deadline) {
      deadline = expires_at;
    }
  }
  return deadline;
}

}  // namespace porch
