"""Moving-average crossover, long only.

Goes long `qty` shares when the fast SMA is above the slow SMA, flat
otherwise. Parameters (set them in the dashboard or with --param):
    fast  (default 10)   slow (default 40)   qty (default 100)
"""
import algo


class SmaCrossover(algo.Strategy):
    def on_start(self, ctx):
        self.fast = int(ctx.param("fast", 10))
        self.slow = int(ctx.param("slow", 40))
        self.qty = ctx.param("qty", 100)
        if self.fast >= self.slow:
            raise ValueError("fast must be smaller than slow")
        ctx.log(f"SMA crossover {self.fast}/{self.slow}, qty {self.qty}")

    def on_bar(self, ctx, bar):
        closes = ctx.closes(bar.symbol, self.slow)
        if len(closes) < self.slow:
            return
        fast = sum(closes[-self.fast:]) / self.fast
        slow = sum(closes) / self.slow
        ctx.target_position(bar.symbol, self.qty if fast > slow else 0)
