#include <gtest/gtest.h>

#include "algo/broker/ibkr_broker.hpp"
#include "algo/data/bar_aggregator.hpp"
#include "algo/data/csv_feed.hpp"
#include "test_helpers.hpp"

using namespace algo;
using algo::test::TempDir;

TEST(Timestamp, ParsesCommonFormats) {
  EXPECT_EQ(parse_timestamp("1700000000"), 1'700'000'000);
  EXPECT_EQ(parse_timestamp("1700000000000"), 1'700'000'000);  // ms
  EXPECT_EQ(parse_timestamp("1970-01-02"), 86'400);
  EXPECT_EQ(parse_timestamp("19700102"), 86'400);
  EXPECT_EQ(parse_timestamp("2024-02-29 13:30:15"), 1'709'213'415);
  EXPECT_EQ(parse_timestamp("2024-02-29T13:30:15Z"), 1'709'213'415);
  EXPECT_EQ(parse_timestamp("2024-02-29 13:30"), 1'709'213'400);
  EXPECT_THROW(parse_timestamp("yesterday"), std::invalid_argument);
  EXPECT_THROW(parse_timestamp("2024-13-01"), std::invalid_argument);
  EXPECT_THROW(parse_timestamp(""), std::invalid_argument);
}

TEST(CsvFeed, LoadsSortsAndDeduplicates) {
  TempDir d;
  auto p = d.write("X.csv",
                   "Date,Open,High,Low,Close,Volume\r\n"
                   "2024-01-03,2,3,1,2.5,10\r\n"
                   "2024-01-02,1,2,0.5,1.5,10\r\n"
                   "2024-01-03,9,9,9,9,9\r\n"
                   "\r\n");
  auto bars = load_csv_bars(p, "X");
  ASSERT_EQ(bars.size(), 2u);
  EXPECT_LT(bars[0].time, bars[1].time);
  EXPECT_DOUBLE_EQ(bars[0].close, 1.5);
  EXPECT_DOUBLE_EQ(bars[1].close, 2.5);  // first occurrence kept
  EXPECT_EQ(bars[0].symbol, "X");
}

TEST(CsvFeed, VolumeIsOptionalAndColumnsAnyOrder) {
  TempDir d;
  auto p = d.write("Y.csv", "close,low,high,open,timestamp\n10,9,11,9.5,1700000000\n");
  auto bars = load_csv_bars(p, "Y");
  ASSERT_EQ(bars.size(), 1u);
  EXPECT_DOUBLE_EQ(bars[0].open, 9.5);
  EXPECT_DOUBLE_EQ(bars[0].volume, 0.0);
}

TEST(CsvFeed, ReportsLineNumberOnBadRow) {
  TempDir d;
  auto p = d.write("Z.csv", "time,open,high,low,close\n1,1,1,1,1\n2,1,abc,1,1\n");
  try {
    load_csv_bars(p, "Z");
    FAIL() << "expected an error";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find(":3:"), std::string::npos) << e.what();
  }
}

TEST(CsvFeed, RejectsInconsistentPricesAndMissingColumns) {
  TempDir d;
  EXPECT_THROW(load_csv_bars(d.write("a.csv", "time,open,high,low,close\n1,1,0.5,1,1\n"), "a"),
               std::runtime_error);  // high < low
  EXPECT_THROW(load_csv_bars(d.write("b.csv", "time,open,high,low,close\n1,0,1,1,1\n"), "b"),
               std::runtime_error);  // zero price
  EXPECT_THROW(load_csv_bars(d.write("c.csv", "time,open,high,close\n1,1,1,1\n"), "c"),
               std::runtime_error);  // no low
  EXPECT_THROW(load_csv_bars((d.path() / "missing.csv").string(), "m"), std::runtime_error);
}

TEST(CsvFeed, DatasetMergesSymbolsInTimeOrder) {
  TempDir d;
  d.write("A.csv", "time,open,high,low,close\n100,1,1,1,1\n300,1,1,1,1\n");
  d.write("B.csv", "time,open,high,low,close\n100,2,2,2,2\n200,2,2,2,2\n");
  d.write("notes.txt", "x");
  auto all = load_csv_dataset(d.str(), {"B", "A"});
  ASSERT_EQ(all.size(), 4u);
  EXPECT_EQ(all[0].symbol, "A");  // tie broken by symbol
  EXPECT_EQ(all[1].symbol, "B");
  EXPECT_EQ(all[2].time, 200);
  EXPECT_EQ(all[3].time, 300);
  EXPECT_EQ(list_csv_symbols(d.str()), (std::vector<std::string>{"A", "B"}));
}

TEST(BarAggregator, RollsFiveSecondBarsIntoMinutes) {
  std::vector<Bar> out;
  BarAggregator agg(60, [&](const Bar& b) { out.push_back(b); });
  const Timestamp t0 = 1'700'000'040;  // aligned to the minute
  for (int i = 0; i < 12; ++i) {
    agg.add(test::bar("A", t0 + 5 * i, 100 + i, 101 + i, 99 + i, 100.5 + i, 10));
  }
  EXPECT_TRUE(out.empty());  // minute not complete until the next one starts
  agg.add(test::bar("A", t0 + 60, 200, 200, 200, 200, 1));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].time, t0);
  EXPECT_DOUBLE_EQ(out[0].open, 100);
  EXPECT_DOUBLE_EQ(out[0].high, 112);
  EXPECT_DOUBLE_EQ(out[0].low, 99);
  EXPECT_DOUBLE_EQ(out[0].close, 111.5);
  EXPECT_DOUBLE_EQ(out[0].volume, 120);
}

TEST(BarAggregator, KeepsSymbolsSeparateAndIgnoresLateBars) {
  std::vector<Bar> out;
  BarAggregator agg(10, [&](const Bar& b) { out.push_back(b); });
  agg.add(test::bar("A", 100, 1, 1, 1, 1));
  agg.add(test::bar("B", 100, 2, 2, 2, 2));
  agg.add(test::bar("A", 110, 3, 3, 3, 3));
  ASSERT_EQ(out.size(), 1u);
  EXPECT_EQ(out[0].symbol, "A");
  agg.add(test::bar("A", 105, 9, 9, 9, 9));  // late: ignored
  agg.flush();
  EXPECT_EQ(out.size(), 3u);
}

TEST(IbkrUtil, BarSizesAndDurations) {
  EXPECT_EQ(ibkr_bar_size(60), "1 min");
  EXPECT_EQ(ibkr_bar_size(86'400), "1 day");
  EXPECT_THROW(ibkr_bar_size(7), std::invalid_argument);
  EXPECT_EQ(ibkr_duration(60, 30), "1800 S");
  EXPECT_EQ(ibkr_duration(200, 60), "4 D");     // 12000 s = 1 session -> 1*7/5 -> 2 + 2
  EXPECT_EQ(ibkr_duration(200, 86'400), "283 D");
  EXPECT_EQ(ibkr_duration(1000, 86'400), "4 Y");
}
