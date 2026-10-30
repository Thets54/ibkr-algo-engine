#pragma once
// Event-driven backtest on historical bars, using the exact Engine and
// StrategyContext used live. For each timestamp t:
//   1. SimBroker matches orders placed at t-1 against the bars of t
//   2. the engine marks positions to the close of t and records equity
//   3. the strategy sees the bars of t (orders it sends wait for t+1)

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

#include "algo/broker/sim_broker.hpp"
#include "algo/core/metrics.hpp"
#include "algo/engine/engine.hpp"

namespace algo {

struct BacktestResult {
  PerformanceMetrics metrics;
  size_t bars_processed = 0;
  bool completed = false;  ///< false if stopped early or the strategy failed
  nlohmann::json snapshot;
};

nlohmann::json metrics_to_json(const PerformanceMetrics& m);

class Backtester {
 public:
  Backtester(EngineOptions options, CommissionConfig commission, StrategyPtr strategy);

  /// progress(fraction in [0,1]) is called roughly every 1% of the data.
  BacktestResult run(const std::vector<Bar>& bars, const std::atomic<bool>* stop_flag = nullptr,
                     const std::function<void(double)>& progress = {});

  [[nodiscard]] Engine& engine() { return *engine_; }

 private:
  SimBroker broker_;
  std::unique_ptr<Engine> engine_;
};

}  // namespace algo
