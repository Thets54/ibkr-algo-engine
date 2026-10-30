#include <gtest/gtest.h>

#include "algo/core/portfolio.hpp"

using namespace algo;

namespace {
Fill fill(const std::string& s, Side side, double q, double p, double c = 0.0) {
  Fill f;
  f.symbol = s;
  f.side = side;
  f.quantity = q;
  f.price = p;
  f.commission = c;
  return f;
}
}  // namespace

TEST(Portfolio, BuyUpdatesCashAndAveragePrice) {
  Portfolio p(10'000);
  p.apply_fill(fill("A", Side::Buy, 10, 100, 1.0));
  p.apply_fill(fill("A", Side::Buy, 10, 110, 1.0));
  EXPECT_DOUBLE_EQ(p.position_qty("A"), 20);
  EXPECT_DOUBLE_EQ(p.positions().at("A").avg_price, 105);
  EXPECT_DOUBLE_EQ(p.cash(), 10'000 - 1000 - 1100 - 2);
  EXPECT_DOUBLE_EQ(p.total_commission(), 2.0);
}

TEST(Portfolio, PartialCloseRealizesPnlAndKeepsAverage) {
  Portfolio p(10'000);
  p.apply_fill(fill("A", Side::Buy, 10, 100));
  p.apply_fill(fill("A", Side::Sell, 4, 110));
  const auto& pos = p.positions().at("A");
  EXPECT_DOUBLE_EQ(pos.quantity, 6);
  EXPECT_DOUBLE_EQ(pos.avg_price, 100);
  EXPECT_DOUBLE_EQ(pos.realized_pnl, 40);
  EXPECT_EQ(p.closed_trades(), 1);
  EXPECT_EQ(p.winning_trades(), 1);
}

TEST(Portfolio, FlipFromLongToShortOpensRemainderAtFillPrice) {
  Portfolio p(10'000);
  p.apply_fill(fill("A", Side::Buy, 10, 100));
  p.apply_fill(fill("A", Side::Sell, 15, 90));
  const auto& pos = p.positions().at("A");
  EXPECT_DOUBLE_EQ(pos.quantity, -5);
  EXPECT_DOUBLE_EQ(pos.avg_price, 90);
  EXPECT_DOUBLE_EQ(pos.realized_pnl, -100);
}

TEST(Portfolio, ShortPositionProfitsWhenPriceFalls) {
  Portfolio p(10'000);
  p.apply_fill(fill("A", Side::Sell, 10, 100));
  p.mark("A", 80);
  EXPECT_DOUBLE_EQ(p.unrealized_pnl(), 200);
  EXPECT_DOUBLE_EQ(p.equity(), 10'200);
  p.apply_fill(fill("A", Side::Buy, 10, 80));
  EXPECT_DOUBLE_EQ(p.position_qty("A"), 0);
  EXPECT_DOUBLE_EQ(p.realized_pnl(), 200);
  EXPECT_DOUBLE_EQ(p.equity(), 10'200);
}

TEST(Portfolio, EquityIsCashPlusMarketValue) {
  Portfolio p(1'000);
  p.apply_fill(fill("A", Side::Buy, 5, 100));
  p.mark("A", 120);
  EXPECT_DOUBLE_EQ(p.equity(), 1'000 + 5 * 20);
  EXPECT_DOUBLE_EQ(p.gross_exposure(), 600);
}

TEST(Portfolio, SetPositionKeepsEquityConsistent) {
  Portfolio p(10'000);
  p.set_position("A", 10, 50);
  EXPECT_DOUBLE_EQ(p.equity(), 10'000);  // seeded at cost
  p.set_position("A", 20, 50);
  EXPECT_DOUBLE_EQ(p.equity(), 10'000);
  EXPECT_DOUBLE_EQ(p.cash(), 9'000);
}

TEST(Portfolio, RejectsNonPositiveFillQuantity) {
  Portfolio p;
  EXPECT_THROW(p.apply_fill(fill("A", Side::Buy, 0, 100)), std::invalid_argument);
}

TEST(Portfolio, RecordEquityOverwritesSameTimestamp) {
  Portfolio p(100);
  p.record_equity(1);
  p.record_equity(1);
  p.record_equity(2);
  EXPECT_EQ(p.equity_curve().size(), 2u);
}
