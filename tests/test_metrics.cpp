#include <gtest/gtest.h>

#include <cmath>

#include "algo/core/metrics.hpp"

using namespace algo;

TEST(Metrics, EmptyCurve) {
  auto m = compute_metrics(std::vector<EquityPoint>{});
  EXPECT_EQ(m.periods, 0);
  EXPECT_DOUBLE_EQ(m.sharpe, 0.0);
}

TEST(Metrics, InfersPeriodsPerYear) {
  std::vector<EquityPoint> daily, minute;
  for (int i = 0; i < 10; ++i) {
    daily.push_back({i * 86'400, 1.0});
    minute.push_back({i * 60, 1.0});
  }
  EXPECT_NEAR(infer_periods_per_year(daily), 252.0, 1e-9);
  EXPECT_NEAR(infer_periods_per_year(minute), 252.0 * 390.0, 1e-6);
}

TEST(Metrics, ReturnDrawdownAndSharpe) {
  // +10%, -20%, +25%: ends at 110
  std::vector<EquityPoint> c = {{0, 100}, {86'400, 110}, {2 * 86'400, 88}, {3 * 86'400, 110}};
  auto m = compute_metrics(c);
  EXPECT_NEAR(m.total_return, 0.10, 1e-12);
  EXPECT_NEAR(m.max_drawdown, 0.20, 1e-12);
  const double r[] = {0.10, -0.20, 0.25};
  const double mean = (r[0] + r[1] + r[2]) / 3;
  double var = 0;
  for (double x : r) var += (x - mean) * (x - mean);
  var /= 2;
  EXPECT_NEAR(m.sharpe, mean / std::sqrt(var) * std::sqrt(252.0), 1e-9);
}

TEST(Metrics, ConstantCurveHasZeroVolAndSharpe) {
  std::vector<EquityPoint> c = {{0, 100}, {86'400, 100}, {2 * 86'400, 100}};
  auto m = compute_metrics(c);
  EXPECT_DOUBLE_EQ(m.annualized_vol, 0.0);
  EXPECT_DOUBLE_EQ(m.sharpe, 0.0);
  EXPECT_DOUBLE_EQ(m.max_drawdown, 0.0);
}
