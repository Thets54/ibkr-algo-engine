#include "algo/broker/sim_broker.hpp"

#include <algorithm>
#include <cmath>

namespace algo {

double ibkr_like_commission(const CommissionConfig& cfg, double quantity, double price) {
  const double value = std::abs(quantity * price);
  double c = std::max(cfg.min_per_order, cfg.per_share * std::abs(quantity));
  if (cfg.max_pct_of_value > 0) c = std::min(c, cfg.max_pct_of_value * value);
  return c;
}

SimBroker::SimBroker(CommissionConfig commission) : commission_(commission) {}

void SimBroker::submit(const Order& order) {
  open_[order.id] = order;
  if (listener_) listener_->on_order_status(order.id, OrderStatus::Submitted, "");
}

void SimBroker::cancel(OrderId id) {
  if (open_.erase(id) > 0 && listener_) {
    listener_->on_order_status(id, OrderStatus::Cancelled, "cancelled");
  }
}

void SimBroker::process_bar(const Bar& bar) {
  std::vector<OrderId> done;
  for (auto& [id, o] : open_) {
    if (o.symbol != bar.symbol) continue;
    double price = 0.0;
    bool fill = false;
    if (o.type == OrderType::Market) {
      const double slip = bar.open * commission_.slippage_bps * 1e-4;
      price = o.side == Side::Buy ? bar.open + slip : bar.open - slip;
      fill = true;
    } else if (o.side == Side::Buy && bar.low <= o.limit_price) {
      price = std::min(bar.open, o.limit_price);
      fill = true;
    } else if (o.side == Side::Sell && bar.high >= o.limit_price) {
      price = std::max(bar.open, o.limit_price);
      fill = true;
    }
    if (!fill) continue;
    Fill f;
    f.order_id = id;
    f.symbol = o.symbol;
    f.side = o.side;
    f.quantity = o.quantity;
    f.price = price;
    f.commission = ibkr_like_commission(commission_, o.quantity, price);
    f.time = bar.time;
    if (listener_) listener_->on_fill(f);
    done.push_back(id);
  }
  for (OrderId id : done) open_.erase(id);
}

}  // namespace algo
