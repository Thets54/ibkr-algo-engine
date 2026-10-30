#pragma once
// ===========================================================================
//  Public strategy API. This is the only header a C++ strategy needs.
//
//    #include <algo/strategy/strategy.hpp>
//
//    class MyStrategy : public algo::Strategy {
//     public:
//      void on_bar(algo::StrategyContext& ctx, const algo::Bar& bar) override {
//        auto h = ctx.history(bar.symbol, 20);
//        if (h.size() < 20) return;
//        ...
//        ctx.target_position(bar.symbol, 100);
//      }
//    };
//    ALGO_EXPORT_STRATEGY(MyStrategy)
//
//  The engine calls every hook from a single thread, so strategies never
//  need locks. Orders sent from on_bar are filled at the earliest on the
//  NEXT bar in backtests (no look-ahead).
// ===========================================================================

#include <string>
#include <vector>

#include "algo/core/types.hpp"

namespace algo {

class StrategyContext {
 public:
  virtual ~StrategyContext() = default;

  // --- orders (all go through the risk manager; 0 means rejected) --------
  virtual OrderId buy(const std::string& symbol, double qty) = 0;
  virtual OrderId sell(const std::string& symbol, double qty) = 0;
  virtual OrderId buy_limit(const std::string& symbol, double qty, double price) = 0;
  virtual OrderId sell_limit(const std::string& symbol, double qty, double price) = 0;
  /// Trades the difference between the current position (including open
  /// orders) and `target_qty` with a market order. Returns 0 if nothing to do.
  virtual OrderId target_position(const std::string& symbol, double target_qty) = 0;
  virtual bool cancel(OrderId id) = 0;
  /// Cancels all open orders (for one symbol, or all if symbol is empty).
  virtual void cancel_all(const std::string& symbol = "") = 0;

  // --- state ---------------------------------------------------------------
  [[nodiscard]] virtual double position(const std::string& symbol) const = 0;
  [[nodiscard]] virtual double avg_price(const std::string& symbol) const = 0;
  /// Quantity of open (unfilled) orders, signed.
  [[nodiscard]] virtual double pending_quantity(const std::string& symbol) const = 0;
  [[nodiscard]] virtual double cash() const = 0;
  [[nodiscard]] virtual double equity() const = 0;
  /// Last `n` completed bars for `symbol`, oldest first, current bar last.
  [[nodiscard]] virtual std::vector<Bar> history(const std::string& symbol, size_t n) const = 0;
  [[nodiscard]] virtual const std::vector<std::string>& symbols() const = 0;
  [[nodiscard]] virtual Timestamp now() const = 0;

  // --- parameters set in the UI / CLI (--param name=value) ---------------
  [[nodiscard]] virtual double param(const std::string& name, double default_value) const = 0;
  [[nodiscard]] virtual std::string param_str(const std::string& name,
                                              const std::string& default_value) const = 0;

  virtual void log(const std::string& message) = 0;
};

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual void on_start(StrategyContext& /*ctx*/) {}
  virtual void on_bar(StrategyContext& ctx, const Bar& bar) = 0;
  virtual void on_fill(StrategyContext& /*ctx*/, const Fill& /*fill*/) {}
  virtual void on_order_rejected(StrategyContext& /*ctx*/, const Order& /*order*/) {}
  virtual void on_stop(StrategyContext& /*ctx*/) {}
};

}  // namespace algo

/// Bumped whenever StrategyContext or Strategy change layout.
#define ALGO_STRATEGY_ABI_VERSION 1

#if defined(_WIN32)
#define ALGO_PLUGIN_API __declspec(dllexport)
#else
#define ALGO_PLUGIN_API __attribute__((visibility("default")))
#endif

/// Exposes a Strategy subclass from a shared library (.so / .dylib / .dll).
#define ALGO_EXPORT_STRATEGY(StrategyClass)                                           \
  extern "C" {                                                                       \
  ALGO_PLUGIN_API int algo_strategy_abi_version() { return ALGO_STRATEGY_ABI_VERSION; } \
  ALGO_PLUGIN_API algo::Strategy* algo_create_strategy() { return new StrategyClass(); } \
  ALGO_PLUGIN_API void algo_destroy_strategy(algo::Strategy* s) { delete s; }        \
  }
