// Engine + backtester behaviour: event ordering, no look-ahead, risk
// integration and failure handling.
#include <gtest/gtest.h>

#include "algo/engine/backtester.hpp"
#include "test_helpers.hpp"

using namespace algo;
using test::LambdaStrategy;

namespace {

EngineOptions opts(std::vector<std::string> symbols = {"A"}) {
  EngineOptions o;
  o.symbols = std::move(symbols);
  o.bar_seconds = 86'400;
  o.starting_cash = 100'000;
  o.risk.max_position_qty = 1e9;
  o.risk.max_position_notional = 1e9;
  o.risk.max_order_notional = 1e9;
  o.risk.max_gross_exposure = 1e9;
  o.risk.max_daily_loss = 1e9;
  o.risk.max_orders_per_minute = 1'000'000;
  return o;
}

CommissionConfig free_trading() {
  CommissionConfig c;
  c.per_share = 0;
  c.min_per_order = 0;
  c.slippage_bps = 0;
  return c;
}

}  // namespace

TEST(Backtest, OrdersFillAtNextBarOpenNotCurrentClose) {
  auto* s = new LambdaStrategy;
  std::vector<Fill> fills;
  int bar_index = 0;
  s->bar_fn = [&](StrategyContext& ctx, const Bar&) {
    if (bar_index++ == 0) ctx.buy("A", 10);
  };
  s->fill_fn = [&](StrategyContext&, const Fill& f) { fills.push_back(f); };
  Backtester bt(opts(), free_trading(), test::own(s));
  std::vector<Bar> bars = {test::bar("A", 0, 10, 11, 9, 10.5), test::bar("A", 86'400, 12, 13, 11, 12.5),
                           test::bar("A", 2 * 86'400, 13, 14, 12, 13.5)};
  auto res = bt.run(bars);
  ASSERT_TRUE(res.completed);
  ASSERT_EQ(fills.size(), 1u);
  EXPECT_DOUBLE_EQ(fills[0].price, 12.0);  // open of bar 2, not close of bar 1
  EXPECT_EQ(fills[0].time, 86'400);
  EXPECT_DOUBLE_EQ(bt.engine().portfolio().equity(), 100'000 + 10 * (13.5 - 12));
  EXPECT_EQ(res.bars_processed, 3u);
}

TEST(Backtest, StrategyNeverSeesFutureBars) {
  auto* s = new LambdaStrategy;
  bool leak = false;
  s->bar_fn = [&](StrategyContext& ctx, const Bar& b) {
    for (const auto& h : ctx.history("A", 1000)) leak |= h.time > b.time;
    leak |= ctx.now() != b.time;
  };
  Backtester bt(opts(), free_trading(), test::own(s));
  bt.run(test::random_walk("A", 50));
  EXPECT_FALSE(leak);
}

TEST(Backtest, MultiSymbolStepsAreSynchronized) {
  auto* s = new LambdaStrategy;
  std::vector<std::pair<std::string, Timestamp>> seen;
  s->bar_fn = [&](StrategyContext&, const Bar& b) { seen.emplace_back(b.symbol, b.time); };
  Backtester bt(opts({"A", "B"}), free_trading(), test::own(s));
  auto a = test::random_walk("A", 3), b = test::random_walk("B", 3, 50);
  std::vector<Bar> all;
  for (int i = 0; i < 3; ++i) {
    all.push_back(a[i]);
    all.push_back(b[i]);
  }
  bt.run(all);
  ASSERT_EQ(seen.size(), 6u);
  for (size_t i = 1; i < seen.size(); ++i) EXPECT_LE(seen[i - 1].second, seen[i].second);
}

TEST(Backtest, TargetPositionAccountsForPendingOrders) {
  auto* s = new LambdaStrategy;
  s->bar_fn = [&](StrategyContext& ctx, const Bar&) {
    ctx.target_position("A", 100);
    ctx.target_position("A", 100);  // second call must not double the order
  };
  Backtester bt(opts(), free_trading(), test::own(s));
  bt.run(test::random_walk("A", 5));
  EXPECT_DOUBLE_EQ(bt.engine().portfolio().position_qty("A"), 100);
}

TEST(Backtest, ResultsAreDeterministic) {
  auto make = [] {
    auto* s = new LambdaStrategy;
    s->bar_fn = [](StrategyContext& ctx, const Bar& b) {
      auto h = ctx.history(b.symbol, 5);
      if (h.size() < 5) return;
      ctx.target_position(b.symbol, b.close > h.front().close ? 50 : -50);
    };
    return test::own(s);
  };
  auto bars = test::random_walk("A", 300);
  Backtester b1(opts(), CommissionConfig{}, make()), b2(opts(), CommissionConfig{}, make());
  auto r1 = b1.run(bars), r2 = b2.run(bars);
  EXPECT_DOUBLE_EQ(r1.metrics.end_equity, r2.metrics.end_equity);
  EXPECT_EQ(r1.metrics.closed_trades, r2.metrics.closed_trades);
  EXPECT_GT(r1.metrics.closed_trades, 0);
}

TEST(Engine, RiskRejectionReturnsZeroAndIsLogged) {
  auto o = opts();
  o.risk.max_position_qty = 5;
  auto* s = new LambdaStrategy;
  OrderId id = -1;
  s->bar_fn = [&](StrategyContext& ctx, const Bar&) { id = ctx.buy("A", 10); };
  Backtester bt(o, free_trading(), test::own(s));
  bt.run({test::bar("A", 0, 10, 10, 10, 10)});
  EXPECT_EQ(id, 0);
  auto snap = bt.engine().snapshot();
  ASSERT_FALSE(snap["orders"].empty());
  EXPECT_EQ(snap["orders"][0]["status"], "Rejected");
}

TEST(Engine, OrdersForUnknownSymbolsAreRejected) {
  auto* s = new LambdaStrategy;
  OrderId id = -1;
  s->bar_fn = [&](StrategyContext& ctx, const Bar&) { id = ctx.buy("NOT_SUBSCRIBED", 1); };
  Backtester bt(opts(), free_trading(), test::own(s));
  bt.run({test::bar("A", 0, 10, 10, 10, 10)});
  EXPECT_EQ(id, 0);
}

TEST(Engine, DailyLossTripsKillSwitchAndFlattens) {
  auto o = opts();
  o.risk.max_daily_loss = 50;
  auto* s = new LambdaStrategy;
  int calls = 0;
  s->bar_fn = [&](StrategyContext& ctx, const Bar&) {
    ++calls;
    ctx.target_position("A", 100);
  };
  Backtester bt(o, free_trading(), test::own(s));
  // Each step is a new day; bar 3 drops the price enough to lose > 50 intraday.
  std::vector<Bar> bars = {test::bar("A", 0, 10, 10, 10, 10), test::bar("A", 86'400, 10, 10, 10, 10),
                           test::bar("A", 86'400 + 3600, 10, 10, 9, 9),  // same UTC day: -100
                           test::bar("A", 86'400 + 7200, 9, 9, 9, 9), test::bar("A", 86'400 + 10'800, 9, 9, 9, 9)};
  auto res = bt.run(bars);
  EXPECT_TRUE(bt.engine().halted());
  EXPECT_EQ(calls, 2);  // strategy not called once halted
  EXPECT_DOUBLE_EQ(bt.engine().portfolio().position_qty("A"), 0.0);  // flattened on the next bar
  EXPECT_TRUE(res.completed);
}

TEST(Engine, StrategyExceptionHaltsAndIsReported) {
  auto* s = new LambdaStrategy;
  s->bar_fn = [](StrategyContext&, const Bar& b) {
    if (b.time > 0) throw std::runtime_error("boom");
  };
  Backtester bt(opts(), free_trading(), test::own(s));
  auto res = bt.run(test::random_walk("A", 10));
  EXPECT_FALSE(res.completed);
  EXPECT_TRUE(bt.engine().strategy_failed());
  bool found = false;
  for (const auto& l : bt.engine().logs()) found |= l.message.find("boom") != std::string::npos;
  EXPECT_TRUE(found);
}

TEST(Engine, ParamsAreTypedWithDefaults) {
  auto o = opts();
  o.params = {{"n", 5}, {"name", "x"}, {"s", "2.5"}, {"flag", true}};
  auto* s = new LambdaStrategy;
  double n = 0, missing = 0, sv = 0, flag = 0;
  std::string name;
  s->start = [&](StrategyContext& ctx) {
    n = ctx.param("n", 1);
    missing = ctx.param("missing", 7);
    sv = ctx.param("s", 0);
    flag = ctx.param("flag", 0);
    name = ctx.param_str("name", "");
  };
  Backtester bt(o, free_trading(), test::own(s));
  bt.run({});
  EXPECT_DOUBLE_EQ(n, 5);
  EXPECT_DOUBLE_EQ(missing, 7);
  EXPECT_DOUBLE_EQ(sv, 2.5);
  EXPECT_DOUBLE_EQ(flag, 1);
  EXPECT_EQ(name, "x");
}

TEST(Engine, SnapshotDownsamplesEquityCurve) {
  auto* s = new LambdaStrategy;
  Backtester bt(opts(), free_trading(), test::own(s));
  bt.run(test::random_walk("A", 1000));
  auto snap = bt.engine().snapshot(100);
  EXPECT_LE(snap["equity_curve"].size(), 102u);
  EXPECT_EQ(snap["equity_curve"].back()[0], 1'700'000'000 + 999 * 86'400);
}
