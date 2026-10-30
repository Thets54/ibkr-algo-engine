#include "algo/data/csv_feed.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <stdexcept>

namespace algo {

namespace fs = std::filesystem;

namespace {

std::string trim(std::string s) {
  auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  if (s.size() >= 2 && s.front() == '"' && s.back() == '"') s = s.substr(1, s.size() - 2);
  return s;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

std::vector<std::string> split(const std::string& line, char sep) {
  std::vector<std::string> out;
  std::string cell;
  std::istringstream ss(line);
  while (std::getline(ss, cell, sep)) out.push_back(trim(cell));
  if (!line.empty() && line.back() == sep) out.emplace_back();
  return out;
}

// Days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant).
std::int64_t days_from_civil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

double to_double(const std::string& s, const char* what) {
  try {
    size_t pos = 0;
    double v = std::stod(s, &pos);
    if (pos != s.size()) throw std::invalid_argument("trailing");
    return v;
  } catch (...) {
    throw std::invalid_argument(std::string("invalid ") + what + " '" + s + "'");
  }
}

}  // namespace

Timestamp parse_timestamp(const std::string& raw) {
  const std::string t = trim(raw);
  if (t.empty()) throw std::invalid_argument("empty timestamp");
  if (std::all_of(t.begin(), t.end(), [](unsigned char c) { return std::isdigit(c); })) {
    std::int64_t v = std::stoll(t);
    if (t.size() == 8) {  // YYYYMMDD (IBKR daily format)
      return days_from_civil(static_cast<int>(v / 10000), static_cast<unsigned>((v / 100) % 100),
                             static_cast<unsigned>(v % 100)) * 86'400;
    }
    if (v > 100'000'000'000LL) v /= 1000;  // milliseconds
    return v;
  }
  int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
  char sep = 0;
  const int n = std::sscanf(t.c_str(), "%4d-%2d-%2d%c%2d:%2d:%2d", &y, &mo, &d, &sep, &h, &mi, &s);
  if (n < 3 || mo < 1 || mo > 12 || d < 1 || d > 31) {
    throw std::invalid_argument("unrecognised timestamp '" + t + "'");
  }
  if (n > 3 && sep != ' ' && sep != 'T') throw std::invalid_argument("bad separator in '" + t + "'");
  if (h < 0 || h > 23 || mi < 0 || mi > 59 || s < 0 || s > 60) {
    throw std::invalid_argument("bad time of day in '" + t + "'");
  }
  return days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86'400 +
         h * 3600 + mi * 60 + s;
}

std::vector<Bar> load_csv_bars(const std::string& path, const std::string& symbol) {
  std::ifstream in(path);
  if (!in) throw std::runtime_error("cannot open " + path);
  std::string line;
  if (!std::getline(in, line)) throw std::runtime_error(path + ": empty file");
  if (!line.empty() && line.back() == '\r') line.pop_back();
  const char sep = line.find(';') != std::string::npos && line.find(',') == std::string::npos ? ';' : ',';

  std::map<std::string, int> col;
  auto header = split(line, sep);
  for (size_t i = 0; i < header.size(); ++i) col[lower(header[i])] = static_cast<int>(i);
  int ti = -1;
  for (const char* k : {"time", "timestamp", "date", "datetime"}) {
    if (col.count(k)) { ti = col[k]; break; }
  }
  for (const char* k : {"open", "high", "low", "close"}) {
    if (!col.count(k)) throw std::runtime_error(path + ": missing column '" + k + "'");
  }
  if (ti < 0) throw std::runtime_error(path + ": missing time/date column");
  const int oi = col["open"], hi = col["high"], li = col["low"], ci = col["close"];
  const int vi = col.count("volume") ? col["volume"] : -1;

  std::vector<Bar> bars;
  int line_no = 1;
  while (std::getline(in, line)) {
    ++line_no;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (trim(line).empty()) continue;
    auto cells = split(line, sep);
    const int need = std::max({ti, oi, hi, li, ci, vi});
    try {
      if (static_cast<int>(cells.size()) <= need) throw std::invalid_argument("too few columns");
      Bar b;
      b.symbol = symbol;
      b.time = parse_timestamp(cells[ti]);
      b.open = to_double(cells[oi], "open");
      b.high = to_double(cells[hi], "high");
      b.low = to_double(cells[li], "low");
      b.close = to_double(cells[ci], "close");
      b.volume = vi >= 0 && !cells[vi].empty() ? to_double(cells[vi], "volume") : 0.0;
      for (double p : {b.open, b.high, b.low, b.close}) {
        if (!(p > 0.0) || !std::isfinite(p)) throw std::invalid_argument("non-positive price");
      }
      if (b.high < b.low) throw std::invalid_argument("high < low");
      bars.push_back(std::move(b));
    } catch (const std::exception& e) {
      throw std::runtime_error(path + ":" + std::to_string(line_no) + ": " + e.what());
    }
  }
  std::stable_sort(bars.begin(), bars.end(), [](const Bar& a, const Bar& b) { return a.time < b.time; });
  bars.erase(std::unique(bars.begin(), bars.end(),
                         [](const Bar& a, const Bar& b) { return a.time == b.time; }),
             bars.end());
  return bars;
}

std::vector<Bar> load_csv_dataset(const std::string& dir, const std::vector<std::string>& symbols) {
  std::vector<Bar> all;
  for (const auto& s : symbols) {
    auto bars = load_csv_bars((fs::path(dir) / (s + ".csv")).string(), s);
    all.insert(all.end(), bars.begin(), bars.end());
  }
  std::stable_sort(all.begin(), all.end(), [](const Bar& a, const Bar& b) {
    return a.time != b.time ? a.time < b.time : a.symbol < b.symbol;
  });
  return all;
}

std::vector<std::string> list_csv_symbols(const std::string& dir) {
  std::vector<std::string> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return out;
  for (const auto& e : fs::directory_iterator(dir, ec)) {
    if (e.is_regular_file() && lower(e.path().extension().string()) == ".csv") {
      out.push_back(e.path().stem().string());
    }
  }
  std::sort(out.begin(), out.end());
  return out;
}

}  // namespace algo
