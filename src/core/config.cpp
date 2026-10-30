#include "algo/core/config.hpp"

#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

namespace algo {

using nlohmann::json;

namespace {
template <typename T>
void get_opt(const json& j, const char* key, T& out) {
  if (j.contains(key) && !j.at(key).is_null()) out = j.at(key).get<T>();
}
}  // namespace

AppConfig config_from_json(const json& j) {
  AppConfig c;
  get_opt(j, "starting_cash", c.starting_cash);
  get_opt(j, "allow_live", c.allow_live);
  if (j.contains("ibkr")) {
    const auto& k = j.at("ibkr");
    get_opt(k, "host", c.ibkr.host);
    get_opt(k, "port", c.ibkr.port);
    get_opt(k, "client_id", c.ibkr.client_id);
    get_opt(k, "exchange", c.ibkr.exchange);
    get_opt(k, "currency", c.ibkr.currency);
    get_opt(k, "market_data_type", c.ibkr.market_data_type);
    get_opt(k, "warmup_bars", c.ibkr.warmup_bars);
  }
  if (j.contains("risk")) {
    const auto& r = j.at("risk");
    get_opt(r, "max_position_qty", c.risk.max_position_qty);
    get_opt(r, "max_position_notional", c.risk.max_position_notional);
    get_opt(r, "max_order_notional", c.risk.max_order_notional);
    get_opt(r, "max_gross_exposure", c.risk.max_gross_exposure);
    get_opt(r, "max_daily_loss", c.risk.max_daily_loss);
    get_opt(r, "max_orders_per_minute", c.risk.max_orders_per_minute);
    get_opt(r, "flatten_on_kill", c.risk.flatten_on_kill);
  }
  if (j.contains("commission")) {
    const auto& m = j.at("commission");
    get_opt(m, "per_share", c.commission.per_share);
    get_opt(m, "min_per_order", c.commission.min_per_order);
    get_opt(m, "max_pct_of_value", c.commission.max_pct_of_value);
    get_opt(m, "slippage_bps", c.commission.slippage_bps);
  }
  if (j.contains("server")) {
    get_opt(j.at("server"), "host", c.server.host);
    get_opt(j.at("server"), "port", c.server.port);
  }
  if (j.contains("paths")) {
    get_opt(j.at("paths"), "strategies_dir", c.paths.strategies_dir);
    get_opt(j.at("paths"), "data_dir", c.paths.data_dir);
    get_opt(j.at("paths"), "cache_dir", c.paths.cache_dir);
  }
  return c;
}

json config_to_json(const AppConfig& c) {
  return json{
      {"starting_cash", c.starting_cash},
      {"allow_live", c.allow_live},
      {"ibkr",
       {{"host", c.ibkr.host},
        {"port", c.ibkr.port},
        {"client_id", c.ibkr.client_id},
        {"exchange", c.ibkr.exchange},
        {"currency", c.ibkr.currency},
        {"market_data_type", c.ibkr.market_data_type},
        {"warmup_bars", c.ibkr.warmup_bars}}},
      {"risk",
       {{"max_position_qty", c.risk.max_position_qty},
        {"max_position_notional", c.risk.max_position_notional},
        {"max_order_notional", c.risk.max_order_notional},
        {"max_gross_exposure", c.risk.max_gross_exposure},
        {"max_daily_loss", c.risk.max_daily_loss},
        {"max_orders_per_minute", c.risk.max_orders_per_minute},
        {"flatten_on_kill", c.risk.flatten_on_kill}}},
      {"commission",
       {{"per_share", c.commission.per_share},
        {"min_per_order", c.commission.min_per_order},
        {"max_pct_of_value", c.commission.max_pct_of_value},
        {"slippage_bps", c.commission.slippage_bps}}},
      {"server", {{"host", c.server.host}, {"port", c.server.port}}},
      {"paths",
       {{"strategies_dir", c.paths.strategies_dir},
        {"data_dir", c.paths.data_dir},
        {"cache_dir", c.paths.cache_dir}}},
  };
}

AppConfig load_config(const std::string& path) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open config file: " + path);
  json j;
  try {
    in >> j;
  } catch (const json::exception& e) {
    throw std::runtime_error("invalid JSON in " + path + ": " + e.what());
  }
  return config_from_json(j);
}

}  // namespace algo
