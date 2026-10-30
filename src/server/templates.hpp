#pragma once
// Starter files offered by the dashboard's "New from template" button.

namespace algo::templates {

inline constexpr const char* kPython = R"PY(import algo


class MyStrategy(algo.Strategy):
    """Moving-average crossover. Parameters can be set from the dashboard."""

    def on_start(self, ctx):
        self.fast = ctx.param("fast", 10)
        self.slow = ctx.param("slow", 30)
        self.qty = ctx.param("qty", 100)
        ctx.log(f"SMA {self.fast}/{self.slow} on {ctx.symbols}")

    def on_bar(self, ctx, bar):
        closes = ctx.closes(bar.symbol, self.slow)
        if len(closes) < self.slow:
            return  # not enough history yet
        fast = sum(closes[-self.fast:]) / self.fast
        slow = sum(closes) / self.slow
        target = self.qty if fast > slow else 0
        ctx.target_position(bar.symbol, target)

    def on_fill(self, ctx, fill):
        ctx.log(f"filled {fill.quantity} {fill.symbol} @ {fill.price:.2f}")
)PY";

inline constexpr const char* kCpp = R"CPP(#include <algo/strategy/strategy.hpp>

#include <numeric>

// Moving-average crossover. Parameters can be set from the dashboard.
class MyStrategy : public algo::Strategy {
 public:
  void on_start(algo::StrategyContext& ctx) override {
    fast_ = static_cast<size_t>(ctx.param("fast", 10));
    slow_ = static_cast<size_t>(ctx.param("slow", 30));
    qty_ = ctx.param("qty", 100);
  }

  void on_bar(algo::StrategyContext& ctx, const algo::Bar& bar) override {
    const auto h = ctx.history(bar.symbol, slow_);
    if (h.size() < slow_) return;  // not enough history yet
    auto mean = [&](size_t n) {
      double s = 0;
      for (size_t i = h.size() - n; i < h.size(); ++i) s += h[i].close;
      return s / static_cast<double>(n);
    };
    ctx.target_position(bar.symbol, mean(fast_) > mean(slow_) ? qty_ : 0.0);
  }

 private:
  size_t fast_ = 10, slow_ = 30;
  double qty_ = 100;
};

ALGO_EXPORT_STRATEGY(MyStrategy)
)CPP";

}  // namespace algo::templates
