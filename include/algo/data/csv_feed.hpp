#pragma once
// Historical bars from CSV files, one file per symbol: <dir>/<SYMBOL>.csv
//
// Header row required. Recognised columns (case-insensitive, any order):
//   time | timestamp | date | datetime, open, high, low, close, volume (optional)
// Time values: unix seconds, "YYYY-MM-DD", "YYYY-MM-DD HH:MM[:SS]" or
// "YYYY-MM-DDTHH:MM[:SS][Z]", all interpreted as UTC.

#include <string>
#include <vector>

#include "algo/core/types.hpp"

namespace algo {

/// Parses a timestamp string; throws std::invalid_argument on bad input.
Timestamp parse_timestamp(const std::string& text);

/// Loads one CSV file. Bars are sorted by time and duplicates removed.
/// Throws std::runtime_error with the line number on malformed rows.
std::vector<Bar> load_csv_bars(const std::string& path, const std::string& symbol);

/// Loads <dir>/<SYMBOL>.csv for each symbol and merges them into one
/// time-ordered stream (ties ordered by symbol for determinism).
std::vector<Bar> load_csv_dataset(const std::string& dir, const std::vector<std::string>& symbols);

/// Lists the symbols available in a data directory (file stems of *.csv).
std::vector<std::string> list_csv_symbols(const std::string& dir);

}  // namespace algo
