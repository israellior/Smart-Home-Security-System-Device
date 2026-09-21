#pragma once

#include <random>

#include "core/ids.h"

namespace porch {

// Version 4 UUIDs. The one implementation of IdSource that is not a test
// double, and the reason IdSource is injected at all.
class RandomIds : public IdSource {
 public:
  RandomIds();
  EventId next() override;

 private:
  std::mt19937_64 engine_;
};

}  // namespace porch
