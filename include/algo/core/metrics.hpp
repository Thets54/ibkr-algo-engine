#pragma once
#include <vector>

#include "algo/core/portfolio.hpp"

namespace algo {

struct PerformanceMetrics {
  double start_equity = 0.0;
  double end_equity = 0.0;
  double total_return = 0.0;       ///< end / start - 1
  double annualized_return = 0.0;  ///< geometric, from the elapsed calendar time
  double annualized_vol = 0.0;
  double sharpe = 0.0;             ///< mean / std of per-period returns, annualized, rf = 0
  double max_drawdown = 0.0;       ///< positive fraction, e.g. 0.12 = -12%
  int closed_trades = 0;
  double win_rate = 0.0;
  double total_commission = 0.0;
  int periods = 0;
  double periods_per_year = 0.0;
};

/// Computes metrics from an equity curve. periods_per_year is inferred from
/// the median spacing of the curve, assuming 252 trading days of 6.5 hours
/// for intraday bars and 252 periods for daily bars.
PerformanceMetrics compute_metrics(const std::vector<EquityPoint>& curve);

/// Same, plus trade statistics from the portfolio.
PerformanceMetrics compute_metrics(const Portfolio& portfolio);

/// Infers how many bars of this spacing fit in one year of trading.
double infer_periods_per_year(const std::vector<EquityPoint>& curve);

}  // namespace algo
