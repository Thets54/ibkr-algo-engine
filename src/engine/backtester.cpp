#include "algo/engine/backtester.hpp"

namespace algo {

nlohmann::json metrics_to_json(const PerformanceMetrics& m) {
  return nlohmann::json{{"start_equity", m.start_equity},
                        {"end_equity", m.end_equity},
                        {"total_return", m.total_return},
                        {"annualized_return", m.annualized_return},
                        {"annualized_vol", m.annualized_vol},
                        {"sharpe", m.sharpe},
                        {"max_drawdown", m.max_drawdown},
                        {"closed_trades", m.closed_trades},
                        {"win_rate", m.win_rate},
                        {"total_commission", m.total_commission},
                        {"periods", m.periods},
                        {"periods_per_year", m.periods_per_year}};
}

Backtester::Backtester(EngineOptions options, CommissionConfig commission, StrategyPtr strategy)
    : broker_(commission) {
  options.wall_clock = false;
  engine_ = std::make_unique<Engine>(std::move(options), broker_, std::move(strategy));
  std::string err;
  broker_.connect(err);
}

BacktestResult Backtester::run(const std::vector<Bar>& bars, const std::atomic<bool>* stop_flag,
                               const std::function<void(double)>& progress) {
  BacktestResult res;
  engine_->start();
  engine_->drain();

  const size_t n = bars.size();
  const size_t report_every = std::max<size_t>(1, n / 100);
  size_t i = 0;
  bool stopped = false;
  while (i < n) {
    if (stop_flag && stop_flag->load()) { stopped = true; break; }
    // All bars sharing a timestamp form one step.
    size_t j = i;
    while (j < n && bars[j].time == bars[i].time) ++j;
    for (size_t k = i; k < j; ++k) broker_.process_bar(bars[k]);  // fills from previous step
    engine_->drain();
    for (size_t k = i; k < j; ++k) engine_->on_bar(bars[k], false);
    engine_->drain();
    // Orders sent in on_bar are acknowledged now; they fill on the next step.
    engine_->drain();
    if (progress && (j / report_every != i / report_every || j == n)) progress(static_cast<double>(j) / n);
    res.bars_processed += j - i;
    i = j;
    if (engine_->strategy_failed()) break;
  }
  engine_->stop();
  engine_->drain();

  res.completed = !stopped && !engine_->strategy_failed();
  res.metrics = compute_metrics(engine_->portfolio());
  res.snapshot = engine_->snapshot();
  res.snapshot["metrics"] = metrics_to_json(res.metrics);
  return res;
}

}  // namespace algo
