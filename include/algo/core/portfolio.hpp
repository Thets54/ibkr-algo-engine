#pragma once
// Local book of cash, positions and PnL. Single-threaded: only the engine
// thread mutates it; other threads read through Engine::snapshot().

#include <map>
#include <string>
#include <vector>

#include "algo/core/types.hpp"

namespace algo {

struct EquityPoint {
  Timestamp time = 0;
  double equity = 0.0;
};

class Portfolio {
 public:
  explicit Portfolio(double starting_cash = 100'000.0);

  /// Applies a fill: updates cash, average price and realized PnL.
  /// Handles increases, partial closes and position flips (long -> short).
  void apply_fill(const Fill& fill);

  /// Seeds an existing broker position (live mode start-up sync).
  void set_position(const std::string& symbol, double quantity, double avg_price);

  /// Updates the mark price used for unrealized PnL and equity.
  void mark(const std::string& symbol, double price);

  /// Appends the current equity to the curve (called once per time step).
  void record_equity(Timestamp t);

  [[nodiscard]] double cash() const { return cash_; }
  [[nodiscard]] double starting_cash() const { return starting_cash_; }
  [[nodiscard]] double equity() const;
  [[nodiscard]] double gross_exposure() const;
  [[nodiscard]] double realized_pnl() const;
  [[nodiscard]] double unrealized_pnl() const;
  [[nodiscard]] double total_commission() const { return total_commission_; }
  [[nodiscard]] double position_qty(const std::string& symbol) const;
  [[nodiscard]] double last_price(const std::string& symbol) const;
  [[nodiscard]] const std::map<std::string, Position>& positions() const { return positions_; }
  [[nodiscard]] const std::vector<EquityPoint>& equity_curve() const { return curve_; }
  [[nodiscard]] int closed_trades() const { return closed_trades_; }
  [[nodiscard]] int winning_trades() const { return winning_trades_; }

 private:
  double starting_cash_;
  double cash_;
  double total_commission_ = 0.0;
  int closed_trades_ = 0;
  int winning_trades_ = 0;
  std::map<std::string, Position> positions_;
  std::vector<EquityPoint> curve_;
};

}  // namespace algo
