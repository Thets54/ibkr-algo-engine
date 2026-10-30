#pragma once
// Pre-trade risk checks and the kill switch. Every order a strategy sends
// goes through check() before it reaches a broker; there is no bypass for
// strategies. Only the engine's own flatten orders skip the limits.

#include <deque>
#include <optional>
#include <string>

#include "algo/core/config.hpp"
#include "algo/core/portfolio.hpp"
#include "algo/core/types.hpp"

namespace algo {

class RiskManager {
 public:
  explicit RiskManager(RiskConfig cfg = {});

  /// Returns a reject reason, or std::nullopt if the order may be sent.
  /// ref_price is the best known price (last bar close) used for notionals.
  [[nodiscard]] std::optional<std::string> check(const Order& order, const Portfolio& portfolio,
                                                 double ref_price, Timestamp now) const;

  /// Must be called for each order actually sent (feeds the rate limiter).
  void record_submit(Timestamp now);

  /// Feeds the current equity. Returns true if this call tripped the kill
  /// switch (daily loss breached). The daily baseline resets on each new
  /// UTC day, which contains the whole US regular session.
  bool update_equity(double equity, Timestamp now);

  void trip(const std::string& reason);
  void reset();

  [[nodiscard]] bool halted() const { return halted_; }
  [[nodiscard]] const std::string& halt_reason() const { return halt_reason_; }
  [[nodiscard]] double daily_pnl() const { return last_equity_ - day_start_equity_; }
  [[nodiscard]] const RiskConfig& config() const { return cfg_; }

 private:
  RiskConfig cfg_;
  bool halted_ = false;
  std::string halt_reason_;
  std::deque<Timestamp> recent_submits_;
  std::int64_t current_day_ = -1;
  double day_start_equity_ = 0.0;
  double last_equity_ = 0.0;
};

}  // namespace algo
