#include "algo/data/bar_aggregator.hpp"

#include <algorithm>
#include <stdexcept>

namespace algo {

BarAggregator::BarAggregator(int bar_seconds, Callback on_complete)
    : bar_seconds_(bar_seconds), on_complete_(std::move(on_complete)) {
  if (bar_seconds_ <= 0) throw std::invalid_argument("bar_seconds must be > 0");
}

void BarAggregator::add(const Bar& b) {
  const Timestamp window = b.time - (((b.time % bar_seconds_) + bar_seconds_) % bar_seconds_);
  auto it = building_.find(b.symbol);
  if (it != building_.end() && it->second.time != window) {
    if (window < it->second.time) return;  // late/out-of-order input: ignore
    on_complete_(it->second);
    building_.erase(it);
    it = building_.end();
  }
  if (it == building_.end()) {
    Bar nb = b;
    nb.time = window;
    building_.emplace(b.symbol, nb);
    return;
  }
  Bar& cur = it->second;
  cur.high = std::max(cur.high, b.high);
  cur.low = std::min(cur.low, b.low);
  cur.close = b.close;
  cur.volume += b.volume;
}

void BarAggregator::flush() {
  for (auto& [_, b] : building_) on_complete_(b);
  building_.clear();
}

}  // namespace algo
