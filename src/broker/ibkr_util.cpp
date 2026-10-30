// SDK-independent helpers of the IBKR adapter (unit-tested without TWS).
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>

namespace algo {

std::string ibkr_bar_size(int s) {
  static const std::map<int, std::string> sizes = {
      {5, "5 secs"},    {10, "10 secs"},   {15, "15 secs"},   {30, "30 secs"},  {60, "1 min"},
      {120, "2 mins"},  {180, "3 mins"},   {300, "5 mins"},   {600, "10 mins"}, {900, "15 mins"},
      {1200, "20 mins"}, {1800, "30 mins"}, {3600, "1 hour"},  {7200, "2 hours"}, {10800, "3 hours"},
      {14400, "4 hours"}, {28800, "8 hours"}, {86400, "1 day"}, {604800, "1 week"}};
  auto it = sizes.find(s);
  if (it == sizes.end()) {
    throw std::invalid_argument("bar size of " + std::to_string(s) +
                                "s is not supported by IBKR (use 5,10,15,30,60,120,180,300,600,900,"
                                "1200,1800,3600,7200,10800,14400,28800,86400 or 604800)");
  }
  return it->second;
}

std::string ibkr_duration(int count, int bar_seconds) {
  if (count <= 0) count = 1;
  constexpr double kSession = 6.5 * 3600.0;  // US regular session
  if (bar_seconds >= 86'400) {
    // Calendar days for `count` trading days (+ weekends/holidays margin).
    const int days = static_cast<int>(std::ceil(count * (bar_seconds / 86'400.0) * 7.0 / 5.0)) + 3;
    if (days > 365) return std::to_string(static_cast<int>(std::ceil(days / 365.0))) + " Y";
    return std::to_string(days) + " D";
  }
  const double seconds = static_cast<double>(count) * bar_seconds;
  if (seconds <= 3'600) return std::to_string(static_cast<int>(seconds)) + " S";
  const int trading_days = static_cast<int>(std::ceil(seconds / kSession));
  const int calendar_days = static_cast<int>(std::ceil(trading_days * 7.0 / 5.0)) + 2;
  return std::to_string(calendar_days) + " D";
}

}  // namespace algo
