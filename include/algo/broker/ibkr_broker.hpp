#pragma once
// Interactive Brokers adapter built on the official TWS API C++ client
// (socket connection to TWS or IB Gateway). Only compiled when the project
// is configured with -DALGO_WITH_IBKR=ON -DTWS_API_DIR=...
//
// Symbols:
//   "AAPL"     -> US stock (STK, SMART routing, USD, configurable)
//   "EUR.USD"  -> forex pair (CASH on IDEALPRO, MIDPOINT bars)
//
// Live bars come from reqRealTimeBars (5-second bars) aggregated to the
// requested bar size. Warm-up history comes from reqHistoricalData.

#include <memory>

#include "algo/broker/broker.hpp"
#include "algo/core/config.hpp"

namespace algo {

class IbkrBroker final : public Broker {
 public:
  /// expect_paper: refuse to connect unless every managed account is a
  /// paper account (IBKR paper account ids start with "D", e.g. DU1234567).
  IbkrBroker(IbkrConfig cfg, bool expect_paper);
  ~IbkrBroker() override;

  [[nodiscard]] std::string name() const override { return "ibkr"; }
  void set_listener(BrokerListener* listener) override;
  bool connect(std::string& error) override;
  void disconnect() override;
  [[nodiscard]] bool is_connected() const override;
  void submit(const Order& order) override;
  void cancel(OrderId id) override;
  void subscribe_bars(const std::vector<std::string>& symbols, int bar_seconds) override;
  void request_history(const std::string& symbol, int count, int bar_seconds) override;
  void request_positions() override;

  class Impl;

 private:
  std::unique_ptr<Impl> impl_;
};

/// Maps a bar length in seconds to an IBKR barSizeSetting ("1 min"...).
/// Throws std::invalid_argument for sizes IBKR does not support.
std::string ibkr_bar_size(int bar_seconds);
/// Duration string covering `count` bars, e.g. "3 D" or "1800 S".
std::string ibkr_duration(int count, int bar_seconds);

}  // namespace algo
