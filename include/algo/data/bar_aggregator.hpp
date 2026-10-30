#pragma once
// Rolls fine bars (e.g. IBKR's 5-second real-time bars) into coarser bars.
// A bar of length N seconds covering [t, t+N) is emitted as soon as the first
// input bar of the next window arrives, so emitted bars are always complete.

#include <functional>
#include <map>
#include <optional>
#include <string>

#include "algo/core/types.hpp"

namespace algo {

class BarAggregator {
 public:
  using Callback = std::function<void(const Bar&)>;
  BarAggregator(int bar_seconds, Callback on_complete);

  void add(const Bar& fine_bar);
  /// Emits partially-built bars (e.g. at shutdown). Optional.
  void flush();

  [[nodiscard]] int bar_seconds() const { return bar_seconds_; }

 private:
  int bar_seconds_;
  Callback on_complete_;
  std::map<std::string, Bar> building_;
};

}  // namespace algo
