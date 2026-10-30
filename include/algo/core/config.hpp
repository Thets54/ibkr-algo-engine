#pragma once
#include <string>

#include <nlohmann/json_fwd.hpp>

namespace algo {

struct IbkrConfig {
  std::string host = "127.0.0.1";
  int port = 4002;        ///< IB Gateway paper. TWS paper = 7497, TWS live = 7496, Gateway live = 4001
  int client_id = 17;
  std::string exchange = "SMART";
  std::string currency = "USD";
  /// 1 = live, 3 = delayed (free). Delayed lets you test without subscriptions.
  int market_data_type = 1;
  /// Bars requested for warm-up before going live (strategy history).
  int warmup_bars = 200;
};

struct RiskConfig {
  double max_position_qty = 1'000.0;          ///< absolute shares per symbol after the order
  double max_position_notional = 50'000.0;    ///< |qty * price| per symbol after the order
  double max_order_notional = 25'000.0;       ///< |qty * price| of a single order
  double max_gross_exposure = 150'000.0;      ///< sum of |position values| after the order
  double max_daily_loss = 2'000.0;            ///< currency units; breach trips the kill switch
  int max_orders_per_minute = 30;
  bool flatten_on_kill = true;                ///< send closing market orders when tripped
};

struct CommissionConfig {
  // Defaults approximate IBKR Pro "Fixed" US stock pricing. Check the
  // current schedule on interactivebrokers.com before trusting backtests.
  double per_share = 0.005;
  double min_per_order = 1.0;
  double max_pct_of_value = 0.01;
  double slippage_bps = 1.0;  ///< applied to market order fills in the simulator
};

struct ServerConfig {
  std::string host = "127.0.0.1";  ///< never expose publicly: uploads are executable code
  int port = 8080;
};

struct PathsConfig {
  std::string strategies_dir = "strategies";
  std::string data_dir = "data";
  std::string cache_dir = ".algo_cache";
};

struct AppConfig {
  IbkrConfig ibkr;
  RiskConfig risk;
  CommissionConfig commission;
  ServerConfig server;
  PathsConfig paths;
  double starting_cash = 100'000.0;
  /// Live trading (real money) must be enabled explicitly in the config file.
  bool allow_live = false;
};

AppConfig load_config(const std::string& path);   ///< throws on invalid JSON
AppConfig config_from_json(const nlohmann::json& j);
nlohmann::json config_to_json(const AppConfig& c);

}  // namespace algo
