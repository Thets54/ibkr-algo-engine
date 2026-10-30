#pragma once
// A threaded broker that behaves like IBKR from the engine's point of view:
// callbacks arrive on its own thread, bars stream continuously, market orders
// fill asynchronously at the last price. Lets us test paper/live sessions
// end to end without TWS.

#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include "algo/broker/broker.hpp"

namespace algo::test {

class FakeLiveBroker final : public Broker {
 public:
  explicit FakeLiveBroker(int interval_ms = 5) : interval_ms_(interval_ms) {}
  ~FakeLiveBroker() override { disconnect(); }

  std::string name() const override { return "fake-live"; }
  void set_listener(BrokerListener* l) override { listener_ = l; }
  bool connect(std::string&) override {
    connected_ = true;
    if (auto* l = listener_.load()) l->on_connection(true, "fake connected");
    return true;
  }
  void disconnect() override {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    connected_ = false;
  }
  bool is_connected() const override { return connected_; }

  void submit(const Order& o) override {
    std::lock_guard lock(mtx_);
    pending_.push_back(o);
  }
  void cancel(OrderId id) override {
    std::lock_guard lock(mtx_);
    for (auto it = pending_.begin(); it != pending_.end(); ++it) {
      if (it->id == id) {
        pending_.erase(it);
        if (auto* l = listener_.load()) l->on_order_status(id, OrderStatus::Cancelled, "");
        return;
      }
    }
  }
  void subscribe_bars(const std::vector<std::string>& symbols, int bar_seconds) override {
    symbols_ = symbols;
    bar_seconds_ = bar_seconds;
    running_ = true;
    thread_ = std::thread([this] { loop(); });
  }
  void request_history(const std::string& symbol, int count, int bar_seconds) override {
    for (int i = count; i > 0; --i) {
      Bar b;
      b.symbol = symbol;
      b.time = 1'000'000 - i * bar_seconds;
      b.open = b.high = b.low = b.close = 100.0;
      if (auto* l = listener_.load()) l->on_bar(b, true);
    }
  }
  void request_positions() override {
    if (auto* l = listener_.load()) l->on_position("UNRELATED", 5, 10);
  }

  std::atomic<int> fills{0};

 private:
  void loop() {
    Timestamp t = 1'000'000;
    double px = 100.0;
    while (running_) {
      std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
      px += (t / bar_seconds_) % 7 < 3 ? 0.5 : -0.4;  // deterministic wiggle
      auto* l = listener_.load();
      if (!l) continue;
      std::deque<Order> todo;
      {
        std::lock_guard lock(mtx_);
        todo.swap(pending_);
      }
      for (const auto& o : todo) {
        Fill f;
        f.order_id = o.id;
        f.symbol = o.symbol;
        f.side = o.side;
        f.quantity = o.quantity;
        f.price = px;
        f.time = t;
        l->on_fill(f);
        ++fills;
      }
      for (const auto& s : symbols_) {
        Bar b;
        b.symbol = s;
        b.time = t;
        b.open = b.high = b.low = b.close = px;
        l->on_bar(b, false);
      }
      t += bar_seconds_;
    }
  }

  int interval_ms_;
  std::atomic<BrokerListener*> listener_{nullptr};
  std::atomic<bool> connected_{false}, running_{false};
  std::vector<std::string> symbols_;
  int bar_seconds_ = 60;
  std::mutex mtx_;
  std::deque<Order> pending_;
  std::thread thread_;
};

}  // namespace algo::test
