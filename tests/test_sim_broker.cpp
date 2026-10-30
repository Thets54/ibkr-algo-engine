#include <gtest/gtest.h>

#include <vector>

#include "algo/broker/sim_broker.hpp"
#include "test_helpers.hpp"

using namespace algo;

namespace {
struct Recorder : BrokerListener {
  std::vector<Fill> fills;
  std::vector<std::pair<OrderId, OrderStatus>> statuses;
  void on_bar(const Bar&, bool) override {}
  void on_fill(const Fill& f) override { fills.push_back(f); }
  void on_order_status(OrderId id, OrderStatus s, const std::string&) override { statuses.emplace_back(id, s); }
  void on_position(const std::string&, double, double) override {}
  void on_connection(bool, const std::string&) override {}
  void on_broker_message(int, const std::string&) override {}
};

Order order(OrderId id, Side side, OrderType type, double q, double limit = 0) {
  Order o;
  o.id = id;
  o.symbol = "A";
  o.side = side;
  o.type = type;
  o.quantity = q;
  o.limit_price = limit;
  return o;
}

CommissionConfig no_cost() {
  CommissionConfig c;
  c.per_share = 0;
  c.min_per_order = 0;
  c.slippage_bps = 0;
  return c;
}
}  // namespace

TEST(SimBroker, MarketOrderFillsAtNextOpenWithSlippage) {
  CommissionConfig c = no_cost();
  c.slippage_bps = 10;  // 0.1%
  SimBroker b(c);
  Recorder r;
  b.set_listener(&r);
  b.submit(order(1, Side::Buy, OrderType::Market, 10));
  b.submit(order(2, Side::Sell, OrderType::Market, 5));
  b.process_bar(test::bar("A", 1, 100, 105, 95, 102));
  ASSERT_EQ(r.fills.size(), 2u);
  EXPECT_DOUBLE_EQ(r.fills[0].price, 100.1);
  EXPECT_DOUBLE_EQ(r.fills[1].price, 99.9);
  EXPECT_EQ(b.open_order_count(), 0u);
}

TEST(SimBroker, OnlyMatchesOrdersOfTheBarSymbol) {
  SimBroker b(no_cost());
  Recorder r;
  b.set_listener(&r);
  b.submit(order(1, Side::Buy, OrderType::Market, 1));
  b.process_bar(test::bar("OTHER", 1, 10, 10, 10, 10));
  EXPECT_TRUE(r.fills.empty());
}

TEST(SimBroker, LimitBuyFillsOnlyWhenLowTouchesAndAtBestPrice) {
  SimBroker b(no_cost());
  Recorder r;
  b.set_listener(&r);
  b.submit(order(1, Side::Buy, OrderType::Limit, 1, 95));
  b.process_bar(test::bar("A", 1, 100, 101, 96, 100));  // low 96 > 95
  EXPECT_TRUE(r.fills.empty());
  b.process_bar(test::bar("A", 2, 100, 101, 94, 100));
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_DOUBLE_EQ(r.fills[0].price, 95);
  b.submit(order(2, Side::Buy, OrderType::Limit, 1, 95));
  b.process_bar(test::bar("A", 3, 90, 92, 89, 91));  // gaps below the limit
  EXPECT_DOUBLE_EQ(r.fills[1].price, 90);
}

TEST(SimBroker, LimitSellFillsWhenHighTouches) {
  SimBroker b(no_cost());
  Recorder r;
  b.set_listener(&r);
  b.submit(order(1, Side::Sell, OrderType::Limit, 1, 105));
  b.process_bar(test::bar("A", 1, 100, 106, 99, 100));
  ASSERT_EQ(r.fills.size(), 1u);
  EXPECT_DOUBLE_EQ(r.fills[0].price, 105);
}

TEST(SimBroker, CancelRemovesOrder) {
  SimBroker b(no_cost());
  Recorder r;
  b.set_listener(&r);
  b.submit(order(1, Side::Buy, OrderType::Limit, 1, 50));
  b.cancel(1);
  b.process_bar(test::bar("A", 1, 40, 40, 40, 40));
  EXPECT_TRUE(r.fills.empty());
  ASSERT_EQ(r.statuses.size(), 2u);
  EXPECT_EQ(r.statuses[1].second, OrderStatus::Cancelled);
}

TEST(Commission, IbkrLikeSchedule) {
  CommissionConfig c;  // 0.005/share, min 1, max 1%
  EXPECT_DOUBLE_EQ(ibkr_like_commission(c, 100, 50), 1.0);     // min applies
  EXPECT_DOUBLE_EQ(ibkr_like_commission(c, 1000, 50), 5.0);    // per share
  EXPECT_DOUBLE_EQ(ibkr_like_commission(c, 1000, 0.1), 1.0);   // capped at 1% of 100
  EXPECT_DOUBLE_EQ(ibkr_like_commission(c, 10, 1), 0.1);       // cap below the minimum
}
