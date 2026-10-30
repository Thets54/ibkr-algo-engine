#include "algo/core/metrics.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace algo {

double infer_periods_per_year(const std::vector<EquityPoint>& curve) {
  if (curve.size() < 2) return 252.0;
  std::vector<double> gaps;
  gaps.reserve(curve.size() - 1);
  for (size_t i = 1; i < curve.size(); ++i) {
    gaps.push_back(static_cast<double>(curve[i].time - curve[i - 1].time));
  }
  std::nth_element(gaps.begin(), gaps.begin() + gaps.size() / 2, gaps.end());
  const double median = gaps[gaps.size() / 2];
  constexpr double kDay = 86'400.0;
  constexpr double kSessionSeconds = 6.5 * 3600.0;
  if (median <= 0.0) return 252.0;
  if (median >= 0.9 * kDay) return 252.0 * kDay / std::max(median, kDay);  // daily or slower
  return 252.0 * kSessionSeconds / median;                                 // intraday
}

PerformanceMetrics compute_metrics(const std::vector<EquityPoint>& curve) {
  PerformanceMetrics m;
  m.periods = static_cast<int>(curve.size());
  if (curve.empty()) return m;
  m.start_equity = curve.front().equity;
  m.end_equity = curve.back().equity;
  if (m.start_equity > 0) m.total_return = m.end_equity / m.start_equity - 1.0;
  m.periods_per_year = infer_periods_per_year(curve);

  std::vector<double> rets;
  rets.reserve(curve.size());
  for (size_t i = 1; i < curve.size(); ++i) {
    if (curve[i - 1].equity > 0) rets.push_back(curve[i].equity / curve[i - 1].equity - 1.0);
  }
  if (rets.size() >= 2) {
    const double mean = std::accumulate(rets.begin(), rets.end(), 0.0) / rets.size();
    double var = 0.0;
    for (double r : rets) var += (r - mean) * (r - mean);
    var /= static_cast<double>(rets.size() - 1);  // sample variance
    const double sd = std::sqrt(var);
    m.annualized_vol = sd * std::sqrt(m.periods_per_year);
    if (sd > 0) m.sharpe = mean / sd * std::sqrt(m.periods_per_year);
  }

  const double years = static_cast<double>(rets.size()) / m.periods_per_year;
  if (years > 0 && m.start_equity > 0 && m.end_equity > 0) {
    m.annualized_return = std::pow(m.end_equity / m.start_equity, 1.0 / years) - 1.0;
  }

  double peak = curve.front().equity;
  for (const auto& p : curve) {
    peak = std::max(peak, p.equity);
    if (peak > 0) m.max_drawdown = std::max(m.max_drawdown, 1.0 - p.equity / peak);
  }
  return m;
}

PerformanceMetrics compute_metrics(const Portfolio& portfolio) {
  auto m = compute_metrics(portfolio.equity_curve());
  m.closed_trades = portfolio.closed_trades();
  m.win_rate = m.closed_trades > 0
                   ? static_cast<double>(portfolio.winning_trades()) / m.closed_trades
                   : 0.0;
  m.total_commission = portfolio.total_commission();
  return m;
}

}  // namespace algo
