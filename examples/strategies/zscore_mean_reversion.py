"""Z-score mean reversion, long/short, with volatility-scaled sizing.

z = (close - mean(close, lookback)) / std(close, lookback)
  enter short when z >  entry, enter long when z < -entry
  exit when |z| < exit

Position size targets `risk_per_trade` currency units of daily volatility,
capped at `max_qty`. Parameters:
    lookback (20)  entry (2.0)  exit (0.5)  risk_per_trade (500)  max_qty (500)
"""
import math

import algo


class ZScoreMeanReversion(algo.Strategy):
    def on_start(self, ctx):
        self.lookback = int(ctx.param("lookback", 20))
        self.entry = float(ctx.param("entry", 2.0))
        self.exit = float(ctx.param("exit", 0.5))
        self.risk = float(ctx.param("risk_per_trade", 500.0))
        self.max_qty = float(ctx.param("max_qty", 500.0))

    def on_bar(self, ctx, bar):
        closes = ctx.closes(bar.symbol, self.lookback)
        if len(closes) < self.lookback:
            return
        mean = sum(closes) / len(closes)
        var = sum((c - mean) ** 2 for c in closes) / (len(closes) - 1)
        sd = math.sqrt(var)
        if sd <= 0:
            return
        z = (bar.close - mean) / sd

        # Size so that one standard deviation move costs about `risk`.
        qty = min(self.max_qty, math.floor(self.risk / sd))
        pos = ctx.position(bar.symbol) + ctx.pending_quantity(bar.symbol)

        if pos == 0:
            if z > self.entry:
                ctx.target_position(bar.symbol, -qty)
            elif z < -self.entry:
                ctx.target_position(bar.symbol, qty)
        elif abs(z) < self.exit:
            ctx.target_position(bar.symbol, 0)

    def on_order_rejected(self, ctx, order):
        ctx.log(f"order {order.id} rejected: {order.reason}")
