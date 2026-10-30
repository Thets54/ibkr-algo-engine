#include "algo/core/portfolio.hpp"

#include <cmath>
#include <stdexcept>

namespace algo {

namespace {
constexpr double kEps = 1e-9;
}

Portfolio::Portfolio(double starting_cash) : starting_cash_(starting_cash), cash_(starting_cash) {}

void Portfolio::apply_fill(const Fill& fill) {
  if (fill.quantity <= 0.0) throw std::invalid_argument("fill quantity must be positive");
  auto& pos = positions_[fill.symbol];
  pos.symbol = fill.symbol;

  const double delta = signed_qty(fill.side, fill.quantity);
  cash_ -= delta * fill.price;
  cash_ -= fill.commission;
  total_commission_ += fill.commission;
  pos.last_price = fill.price;

  const double old_qty = pos.quantity;
  const bool same_direction = old_qty == 0.0 || (old_qty > 0) == (delta > 0);

  if (same_direction) {
    // Increasing (or opening) a position: weighted average entry price.
    const double new_qty = old_qty + delta;
    pos.avg_price = (old_qty * pos.avg_price + delta * fill.price) / new_qty;
    pos.quantity = new_qty;
    return;
  }

  // Reducing, closing or flipping.
  const double closing = std::min(std::abs(delta), std::abs(old_qty));
  const double direction = old_qty > 0 ? 1.0 : -1.0;
  const double pnl = closing * (fill.price - pos.avg_price) * direction;
  pos.realized_pnl += pnl;
  ++closed_trades_;
  if (pnl > 0) ++winning_trades_;

  const double new_qty = old_qty + delta;
  if (std::abs(new_qty) < kEps) {
    pos.quantity = 0.0;
    pos.avg_price = 0.0;
  } else if ((new_qty > 0) == (old_qty > 0)) {
    pos.quantity = new_qty;  // partial close: avg price unchanged
  } else {
    pos.quantity = new_qty;  // flipped: the remainder opens at the fill price
    pos.avg_price = fill.price;
  }
}

void Portfolio::set_position(const std::string& symbol, double quantity, double avg_price) {
  auto& pos = positions_[symbol];
  // The broker reports the position as already paid for, so cash moves too:
  // this keeps equity = cash + market value consistent from the first bar.
  cash_ += pos.quantity * pos.avg_price;
  pos.symbol = symbol;
  pos.quantity = quantity;
  pos.avg_price = avg_price;
  if (pos.last_price == 0.0) pos.last_price = avg_price;
  cash_ -= quantity * avg_price;
}

void Portfolio::mark(const std::string& symbol, double price) {
  auto it = positions_.find(symbol);
  if (it != positions_.end()) it->second.last_price = price;
}

void Portfolio::record_equity(Timestamp t) {
  const double eq = equity();
  if (!curve_.empty() && curve_.back().time == t) {
    curve_.back().equity = eq;
  } else {
    curve_.push_back({t, eq});
  }
}

double Portfolio::equity() const {
  double eq = cash_;
  for (const auto& [_, p] : positions_) eq += p.market_value();
  return eq;
}

double Portfolio::gross_exposure() const {
  double g = 0.0;
  for (const auto& [_, p] : positions_) g += std::abs(p.market_value());
  return g;
}

double Portfolio::realized_pnl() const {
  double r = 0.0;
  for (const auto& [_, p] : positions_) r += p.realized_pnl;
  return r;
}

double Portfolio::unrealized_pnl() const {
  double u = 0.0;
  for (const auto& [_, p] : positions_) u += p.unrealized_pnl();
  return u;
}

double Portfolio::position_qty(const std::string& symbol) const {
  auto it = positions_.find(symbol);
  return it == positions_.end() ? 0.0 : it->second.quantity;
}

double Portfolio::last_price(const std::string& symbol) const {
  auto it = positions_.find(symbol);
  return it == positions_.end() ? 0.0 : it->second.last_price;
}

}  // namespace algo
