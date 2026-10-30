#pragma once
// Broker abstraction. The engine only talks to this interface, so the same
// strategy runs unchanged against the backtest simulator or IBKR.
//
// Threading contract: a Broker may invoke BrokerListener callbacks from any
// thread (IBKR uses its own reader thread). The engine's listener only
// enqueues events, so callbacks must be cheap and must not block.

#include <string>
#include <vector>

#include "algo/core/types.hpp"

namespace algo {

class BrokerListener {
 public:
  virtual ~BrokerListener() = default;
  /// historical = true for warm-up bars that must not trigger trading.
  virtual void on_bar(const Bar& bar, bool historical) = 0;
  virtual void on_fill(const Fill& fill) = 0;
  virtual void on_order_status(OrderId id, OrderStatus status, const std::string& reason) = 0;
  virtual void on_position(const std::string& symbol, double quantity, double avg_price) = 0;
  virtual void on_connection(bool connected, const std::string& message) = 0;
  virtual void on_broker_message(int code, const std::string& message) = 0;
};

class Broker {
 public:
  virtual ~Broker() = default;
  [[nodiscard]] virtual std::string name() const = 0;
  virtual void set_listener(BrokerListener* listener) = 0;

  /// Blocking connect. Returns false and fills `error` on failure.
  virtual bool connect(std::string& error) = 0;
  virtual void disconnect() = 0;
  [[nodiscard]] virtual bool is_connected() const = 0;

  /// The order id is assigned by the engine and is unique per session.
  virtual void submit(const Order& order) = 0;
  virtual void cancel(OrderId id) = 0;

  /// Streams completed bars of `bar_seconds` for each symbol.
  virtual void subscribe_bars(const std::vector<std::string>& symbols, int bar_seconds) = 0;
  /// Requests the last `count` bars; delivered through on_bar(historical=true).
  virtual void request_history(const std::string& symbol, int count, int bar_seconds) = 0;
  /// Requests current account positions; delivered through on_position().
  virtual void request_positions() = 0;
};

}  // namespace algo
