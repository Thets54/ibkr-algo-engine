#include "algo/risk/risk_manager.hpp"

#include <cmath>
#include <sstream>

namespace algo {

namespace {
std::string fmt(double v) {
  std::ostringstream os;
  os.precision(2);
  os << std::fixed << v;
  return os.str();
}
constexpr Timestamp kMinute = 60;
}  // namespace

RiskManager::RiskManager(RiskConfig cfg) : cfg_(cfg) {}

std::optional<std::string> RiskManager::check(const Order& order, const Portfolio& portfolio,
                                               double ref_price, Timestamp now) const {
  if (halted_) return "kill switch active: " + halt_reason_;
  if (!(order.quantity > 0.0) || !std::isfinite(order.quantity)) return "quantity must be > 0";
  if (order.type == OrderType::Limit && !(order.limit_price > 0.0)) {
    return "limit price must be > 0";
  }
  const double px = order.type == OrderType::Limit ? order.limit_price : ref_price;
  if (!(px > 0.0)) return "no reference price yet for " + order.symbol;

  // Rate limit applies to every order, including risk-reducing ones.
  int in_window = 0;
  for (Timestamp t : recent_submits_) {
    if (now - t < kMinute) ++in_window;
  }
  if (in_window >= cfg_.max_orders_per_minute) {
    return "rate limit: max " + std::to_string(cfg_.max_orders_per_minute) + " orders/minute";
  }

  const double before = portfolio.position_qty(order.symbol);
  const double after = before + signed_qty(order.side, order.quantity);
  const bool reduces_risk = std::abs(after) < std::abs(before) && (after == 0.0 || (after > 0) == (before > 0));
  if (reduces_risk) return std::nullopt;  // always allow de-risking

  const double order_notional = order.quantity * px;
  if (order_notional > cfg_.max_order_notional) {
    return "order notional " + fmt(order_notional) + " > max " + fmt(cfg_.max_order_notional);
  }
  if (std::abs(after) > cfg_.max_position_qty) {
    return "position " + fmt(after) + " > max qty " + fmt(cfg_.max_position_qty);
  }
  if (std::abs(after) * px > cfg_.max_position_notional) {
    return "position notional " + fmt(std::abs(after) * px) + " > max " +
           fmt(cfg_.max_position_notional);
  }
  const double gross_after = portfolio.gross_exposure() - std::abs(before) * px + std::abs(after) * px;
  if (gross_after > cfg_.max_gross_exposure) {
    return "gross exposure " + fmt(gross_after) + " > max " + fmt(cfg_.max_gross_exposure);
  }
  return std::nullopt;
}

void RiskManager::record_submit(Timestamp now) {
  recent_submits_.push_back(now);
  while (!recent_submits_.empty() && now - recent_submits_.front() >= kMinute) {
    recent_submits_.pop_front();
  }
}

bool RiskManager::update_equity(double equity, Timestamp now) {
  const std::int64_t day = now / 86'400;
  if (day != current_day_) {
    current_day_ = day;
    day_start_equity_ = equity;
  }
  last_equity_ = equity;
  if (!halted_ && day_start_equity_ - equity > cfg_.max_daily_loss) {
    trip("daily loss " + fmt(day_start_equity_ - equity) + " > max " + fmt(cfg_.max_daily_loss));
    return true;
  }
  return false;
}

void RiskManager::trip(const std::string& reason) {
  halted_ = true;
  halt_reason_ = reason;
}

void RiskManager::reset() {
  halted_ = false;
  halt_reason_.clear();
  recent_submits_.clear();
}

}  // namespace algo
