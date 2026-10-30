#pragma once
// Backtest broker. Orders are matched against the NEXT bar of their symbol:
//   market: fill at next open +/- slippage
//   limit buy:  fills if next low  <= limit, at min(open, limit)
//   limit sell: fills if next high >= limit, at max(open, limit)
// This is deliberately conservative: no intrabar fills on the bar that
// generated the signal, so there is no look-ahead.

#include <map>
#include <vector>

#include "algo/broker/broker.hpp"
#include "algo/core/config.hpp"

namespace algo {

double ibkr_like_commission(const CommissionConfig& cfg, double quantity, double price);

class SimBroker final : public Broker {
 public:
  explicit SimBroker(CommissionConfig commission = {});

  [[nodiscard]] std::string name() const override { return "simulator"; }
  void set_listener(BrokerListener* l) override { listener_ = l; }
  bool connect(std::string& /*error*/) override { connected_ = true; return true; }
  void disconnect() override { connected_ = false; }
  [[nodiscard]] bool is_connected() const override { return connected_; }
  void submit(const Order& order) override;
  void cancel(OrderId id) override;
  void subscribe_bars(const std::vector<std::string>&, int) override {}
  void request_history(const std::string&, int, int) override {}
  void request_positions() override {}

  /// Called by the backtester with each new bar BEFORE the strategy sees it.
  void process_bar(const Bar& bar);

  [[nodiscard]] size_t open_order_count() const { return open_.size(); }

 private:
  CommissionConfig commission_;
  BrokerListener* listener_ = nullptr;
  bool connected_ = false;
  std::map<OrderId, Order> open_;
};

}  // namespace algo
