#pragma once
// Plain data types shared by every layer (engine, brokers, strategies, UI).
// Kept free of dependencies so strategy plugins can include them cheaply.

#include <cstdint>
#include <string>

namespace algo {

/// Unix time in seconds (UTC). Bars are stamped with their *open* time.
using Timestamp = std::int64_t;
using OrderId = std::int64_t;

struct Bar {
  std::string symbol;
  Timestamp time = 0;  ///< bar open time, seconds since epoch (UTC)
  double open = 0.0;
  double high = 0.0;
  double low = 0.0;
  double close = 0.0;
  double volume = 0.0;
};

enum class Side { Buy, Sell };
enum class OrderType { Market, Limit };

enum class OrderStatus {
  PendingSubmit,  ///< accepted by the engine, not yet acknowledged
  Submitted,      ///< live at the broker
  Filled,
  Cancelled,
  Rejected,       ///< refused by the risk manager or the broker
};

struct Order {
  OrderId id = 0;
  std::string symbol;
  Side side = Side::Buy;
  OrderType type = OrderType::Market;
  double quantity = 0.0;     ///< always positive; direction is given by side
  double limit_price = 0.0;  ///< only for OrderType::Limit
  OrderStatus status = OrderStatus::PendingSubmit;
  double filled_quantity = 0.0;
  double avg_fill_price = 0.0;
  Timestamp created_at = 0;
  std::string reason;        ///< reject reason or free-form tag
};

struct Fill {
  OrderId order_id = 0;
  std::string symbol;
  Side side = Side::Buy;
  double quantity = 0.0;  ///< positive
  double price = 0.0;
  double commission = 0.0;
  Timestamp time = 0;
};

struct Position {
  std::string symbol;
  double quantity = 0.0;   ///< signed: >0 long, <0 short
  double avg_price = 0.0;  ///< average entry price of the open quantity
  double last_price = 0.0;
  double realized_pnl = 0.0;

  [[nodiscard]] double market_value() const { return quantity * last_price; }
  [[nodiscard]] double unrealized_pnl() const { return quantity * (last_price - avg_price); }
};

inline const char* to_string(Side s) { return s == Side::Buy ? "BUY" : "SELL"; }
inline const char* to_string(OrderType t) { return t == OrderType::Market ? "MKT" : "LMT"; }
inline const char* to_string(OrderStatus s) {
  switch (s) {
    case OrderStatus::PendingSubmit: return "PendingSubmit";
    case OrderStatus::Submitted: return "Submitted";
    case OrderStatus::Filled: return "Filled";
    case OrderStatus::Cancelled: return "Cancelled";
    case OrderStatus::Rejected: return "Rejected";
  }
  return "Unknown";
}
inline double signed_qty(Side s, double qty) { return s == Side::Buy ? qty : -qty; }

}  // namespace algo
