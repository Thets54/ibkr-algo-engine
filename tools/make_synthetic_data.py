#!/usr/bin/env python3
"""Generate synthetic OHLCV CSV files for demos and tests.

The prices are NOT real market data: a geometric Brownian motion with a
regime-switching drift (trend / mean-reverting phases) so that both trend
and mean-reversion strategies have something to trade.

    python3 tools/make_synthetic_data.py --out examples/data --days 756
"""
import argparse
import csv
import datetime as dt
import math
import os
import random


def business_days(start: dt.date, n: int):
    d = start
    out = []
    while len(out) < n:
        if d.weekday() < 5:
            out.append(d)
        d += dt.timedelta(days=1)
    return out


def simulate(symbol: str, days, seed: int, s0: float, vol: float):
    rng = random.Random(seed)
    price = s0
    drift = 0.0
    rows = []
    for d in days:
        if rng.random() < 0.02:  # regime switch roughly every 50 days
            drift = rng.choice([-0.0015, 0.0, 0.0, 0.0015])
        ret = drift + vol * rng.gauss(0.0, 1.0)
        open_ = price * math.exp(0.25 * vol * rng.gauss(0.0, 1.0))
        close = price * math.exp(ret)
        high = max(open_, close) * math.exp(abs(0.5 * vol * rng.gauss(0.0, 1.0)))
        low = min(open_, close) * math.exp(-abs(0.5 * vol * rng.gauss(0.0, 1.0)))
        volume = int(1_000_000 * math.exp(0.3 * rng.gauss(0.0, 1.0)))
        rows.append((d.isoformat(), open_, high, low, close, volume))
        price = close
    return rows


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default="examples/data")
    ap.add_argument("--days", type=int, default=756, help="number of business days (756 = ~3 years)")
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    os.makedirs(args.out, exist_ok=True)
    days = business_days(dt.date(2022, 1, 3), args.days)
    specs = [("SYNA", 100.0, 0.018), ("SYNB", 50.0, 0.022), ("SYNC", 250.0, 0.012)]
    for i, (sym, s0, vol) in enumerate(specs):
        rows = simulate(sym, days, args.seed + i, s0, vol)
        path = os.path.join(args.out, f"{sym}.csv")
        with open(path, "w", newline="") as f:
            w = csv.writer(f)
            w.writerow(["date", "open", "high", "low", "close", "volume"])
            for r in rows:
                w.writerow([r[0], f"{r[1]:.4f}", f"{r[2]:.4f}", f"{r[3]:.4f}", f"{r[4]:.4f}", r[5]])
        print(f"wrote {path} ({len(rows)} bars)")


if __name__ == "__main__":
    main()
