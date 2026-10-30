#include <gtest/gtest.h>

#include "algo/risk/risk_manager.hpp"

using namespace algo;

namespace {
Order mkt(const std::string& s, Side side, double q) {
  Order o;
  o.symbol = s;
  o.side = side;
  o.quantity = q;
  return o;
}
RiskConfig cfg() {
  RiskConfig c;
  c.max_position_qty = 100;
  c.max_position_notional = 10'000;
  c.max_order_notional = 5'000;
  c.max_gross_exposure = 15'000;
  c.max_daily_loss = 500;
  c.max_orders_per_minute = 3;
  return c;
}
Fill fill(const std::string& s, Side side, double q, double p) {
  Fill f;
  f.symbol = s;
  f.side = side;
  f.quantity = q;
  f.price = p;
  return f;
}
}  // namespace

TEST(Risk, AcceptsOrderWithinLimits) {
  RiskManager r(cfg());
  Portfolio p;
  EXPECT_FALSE(r.check(mkt("A", Side::Buy, 10), p, 100, 0).has_value());
}

TEST(Risk, RejectsInvalidQuantityAndMissingPrice) {
  RiskManager r(cfg());
  Portfolio p;
  EXPECT_TRUE(r.check(mkt("A", Side::Buy, 0), p, 100, 0).has_value());
  EXPECT_TRUE(r.check(mkt("A", Side::Buy, -1), p, 100, 0).has_value());
  EXPECT_TRUE(r.check(mkt("A", Side::Buy, 1), p, 0, 0).has_value());
  Order lmt = mkt("A", Side::Buy, 1);
  lmt.type = OrderType::Limit;
  EXPECT_TRUE(r.check(lmt, p, 100, 0).has_value());
}

TEST(Risk, EnforcesOrderNotional) {
  RiskManager r(cfg());
  Portfolio p;
  auto why = r.check(mkt("A", Side::Buy, 60), p, 100, 0);  // 6000 > 5000
  ASSERT_TRUE(why.has_value());
  EXPECT_NE(why->find("order notional"), std::string::npos);
}

TEST(Risk, EnforcesPositionQtyIncludingExistingPosition) {
  auto c = cfg();
  c.max_order_notional = 1e9;
  c.max_position_notional = 1e9;
  c.max_gross_exposure = 1e9;
  RiskManager r(c);
  Portfolio p(1e6);
  p.apply_fill(fill("A", Side::Buy, 90, 10));
  EXPECT_FALSE(r.check(mkt("A", Side::Buy, 10), p, 10, 0).has_value());
  EXPECT_TRUE(r.check(mkt("A", Side::Buy, 11), p, 10, 0).has_value());
}

TEST(Risk, AlwaysAllowsRiskReducingOrders) {
  RiskManager r(cfg());
  Portfolio p(1e6);
  p.apply_fill(fill("A", Side::Buy, 100, 200));  // already above notional limit
  EXPECT_FALSE(r.check(mkt("A", Side::Sell, 50), p, 200, 0).has_value());
  // But flipping beyond flat is a new risk-taking order and is checked.
  EXPECT_TRUE(r.check(mkt("A", Side::Sell, 250), p, 200, 0).has_value());
}

TEST(Risk, EnforcesGrossExposureAcrossSymbols) {
  RiskManager r(cfg());
  Portfolio p(1e6);
  p.apply_fill(fill("A", Side::Buy, 90, 100));  // 9000
  p.mark("A", 100);
  // B: 40 * 100 = 4000 -> gross 13000 OK; 60 * 100 = 6000 -> order notional fails first
  EXPECT_FALSE(r.check(mkt("B", Side::Buy, 40), p, 100, 0).has_value());
  p.apply_fill(fill("B", Side::Buy, 40, 100));
  auto why = r.check(mkt("C", Side::Buy, 30), p, 100, 0);  // 16000 > 15000
  ASSERT_TRUE(why.has_value());
  EXPECT_NE(why->find("gross"), std::string::npos);
}

TEST(Risk, RateLimitsOrdersPerMinute) {
  RiskManager r(cfg());
  Portfolio p;
  for (int i = 0; i < 3; ++i) {
    ASSERT_FALSE(r.check(mkt("A", Side::Buy, 1), p, 100, 10).has_value());
    r.record_submit(10);
  }
  EXPECT_TRUE(r.check(mkt("A", Side::Buy, 1), p, 100, 30).has_value());
  EXPECT_FALSE(r.check(mkt("A", Side::Buy, 1), p, 100, 71).has_value());  // window passed
}

TEST(Risk, DailyLossTripsKillSwitchAndBlocksOrders) {
  RiskManager r(cfg());
  Portfolio p;
  const Timestamp day = 1'700'000'000 / 86'400 * 86'400;
  EXPECT_FALSE(r.update_equity(100'000, day + 100));
  EXPECT_FALSE(r.update_equity(99'600, day + 200));
  EXPECT_TRUE(r.update_equity(99'400, day + 300));
  EXPECT_TRUE(r.halted());
  EXPECT_FALSE(r.update_equity(99'000, day + 400));  // only trips once
  auto why = r.check(mkt("A", Side::Buy, 1), p, 100, day + 500);
  ASSERT_TRUE(why.has_value());
  EXPECT_NE(why->find("kill switch"), std::string::npos);
  r.reset();
  EXPECT_FALSE(r.halted());
}

TEST(Risk, DailyBaselineResetsOnNewDay) {
  RiskManager r(cfg());
  const Timestamp day = 1'700'000'000 / 86'400 * 86'400;
  r.update_equity(100'000, day);
  r.update_equity(99'600, day + 1000);
  EXPECT_FALSE(r.update_equity(99'200, day + 86'400));  // new baseline
  EXPECT_DOUBLE_EQ(r.daily_pnl(), 0.0);
}
