// Donchian channel breakout with an ATR trailing stop (C++ example).
//
// Long when the close breaks above the highest high of the last `entry`
// bars, short below the lowest low. Exit on the opposite `exit`-bar
// channel or when price moves `stop_atr` ATRs against the position.
//
// Parameters: entry (20), exit (10), atr (14), stop_atr (3), qty (100)
// Upload this .cpp file in the dashboard: it is compiled automatically.

#include <algo/strategy/strategy.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <string>

class DonchianBreakout : public algo::Strategy {
 public:
  void on_start(algo::StrategyContext& ctx) override {
    entry_ = static_cast<size_t>(ctx.param("entry", 20));
    exit_ = static_cast<size_t>(ctx.param("exit", 10));
    atr_n_ = static_cast<size_t>(ctx.param("atr", 14));
    stop_atr_ = ctx.param("stop_atr", 3.0);
    qty_ = ctx.param("qty", 100);
  }

  void on_bar(algo::StrategyContext& ctx, const algo::Bar& bar) override {
    const size_t need = std::max({entry_, exit_, atr_n_}) + 1;
    const auto h = ctx.history(bar.symbol, need);
    if (h.size() < need) return;

    // Channels use the bars BEFORE the current one.
    auto highest = [&](size_t n) {
      double v = 0;
      for (size_t i = h.size() - 1 - n; i < h.size() - 1; ++i) v = std::max(v, h[i].high);
      return v;
    };
    auto lowest = [&](size_t n) {
      double v = 1e300;
      for (size_t i = h.size() - 1 - n; i < h.size() - 1; ++i) v = std::min(v, h[i].low);
      return v;
    };
    double atr = 0;
    for (size_t i = h.size() - atr_n_; i < h.size(); ++i) {
      const double prev = h[i - 1].close;
      atr += std::max({h[i].high - h[i].low, std::abs(h[i].high - prev), std::abs(h[i].low - prev)});
    }
    atr /= static_cast<double>(atr_n_);

    const double pos = ctx.position(bar.symbol) + ctx.pending_quantity(bar.symbol);
    double& extreme = extremes_[bar.symbol];

    if (pos == 0) {
      if (bar.close > highest(entry_)) {
        ctx.target_position(bar.symbol, qty_);
        extreme = bar.close;
      } else if (bar.close < lowest(entry_)) {
        ctx.target_position(bar.symbol, -qty_);
        extreme = bar.close;
      }
      return;
    }
    if (pos > 0) {
      extreme = std::max(extreme, bar.close);
      if (bar.close < lowest(exit_) || bar.close < extreme - stop_atr_ * atr) {
        ctx.target_position(bar.symbol, 0);
      }
    } else {
      extreme = std::min(extreme, bar.close);
      if (bar.close > highest(exit_) || bar.close > extreme + stop_atr_ * atr) {
        ctx.target_position(bar.symbol, 0);
      }
    }
  }

  void on_fill(algo::StrategyContext& ctx, const algo::Fill& f) override {
    ctx.log(std::string(algo::to_string(f.side)) + " " + std::to_string(f.quantity) + " " + f.symbol);
  }

 private:
  size_t entry_ = 20, exit_ = 10, atr_n_ = 14;
  double stop_atr_ = 3.0, qty_ = 100;
  std::map<std::string, double> extremes_;
};

ALGO_EXPORT_STRATEGY(DonchianBreakout)
