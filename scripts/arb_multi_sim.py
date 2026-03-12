#!/usr/bin/env python3
"""
arb_multi_sim.py – Multi-latency, fee-aware cross-venue arbitrage simulation.

Streams persisted venue JSONL data, detects cross-venue arbitrage windows, and
evaluates each window simultaneously under multiple one-way latency scenarios
with configurable per-venue taker-fee schedules and initial-capital constraints.

Both order legs are assumed to be taker orders (lifting asks and hitting bids).
Net P&L = gross_spread × qty  –  buy_taker_fee  –  sell_taker_fee.
Fill qty is capped by available level-0 liquidity, --target-qty (if set), and
remaining capital (buy_price × qty).

Output
------
  • Side-by-side scenario comparison table printed to stdout.
  • One CSV per latency scenario written to --output-dir.

Usage
-----
    python scripts/arb_multi_sim.py \\
        --input persist/pop-data/2026-03-05-20-27-39 \\
        --symbol BTCUSDT \\
        --latencies-us 100 250 500 1000 2500 \\
        --initial-quote 50000 \\
        --initial-base 1.0 \\
        --target-qty 0.1 \\
        --min-spread-bps 2 \\
        --output-dir sim_results/

Fee config JSON  (--fee-config fees.json)
-----------------------------------------
    {
        "binance": {"taker_bps": 4.0, "maker_bps": 2.0},
        "okx":     {"taker_bps": 5.0, "maker_bps": 2.0},
        "bybit":   {"taker_bps": 5.5, "maker_bps": 2.0},
        "kucoin":  {"taker_bps": 5.0, "maker_bps": 2.5}
    }
"""

from __future__ import annotations

import argparse
import csv
import heapq
import json
import os
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from unified_arb_stream import (
    BookState,
    TimeMergedBookStream,
    UnifiedOrderbook,
    detect_crosses,
    NS_PER_US,
    NS_PER_MS,
    ns_to_dt,
    ns_to_us,
    ns_to_ms,
)
from jsonl_stream_loader import normalize_venue_name


# ═══════════════════════════════════════════════════════════════════════════
# Fee model
# ═══════════════════════════════════════════════════════════════════════════

@dataclass
class FeeConfig:
    """Taker and maker fee rates for one venue."""
    taker_bps: float = 5.0
    maker_bps: float = 2.0

    @property
    def taker_rate(self) -> float:
        return self.taker_bps / 10_000.0

    @property
    def maker_rate(self) -> float:
        return self.maker_bps / 10_000.0


DEFAULT_FEES: dict[str, FeeConfig] = {
    "binance": FeeConfig(taker_bps=0.0,  maker_bps=2.0),
    "okx":     FeeConfig(taker_bps=0.0,  maker_bps=2.0),
    "bybit":   FeeConfig(taker_bps=0.5,  maker_bps=2.0),
    "bitget":  FeeConfig(taker_bps=0.0,  maker_bps=2.0),
    "kucoin":  FeeConfig(taker_bps=0.0,  maker_bps=2.5),
}


def load_fee_schedule(path: Optional[Path]) -> dict[str, FeeConfig]:
    """Return per-venue FeeConfig merged over the built-in defaults."""
    schedule = dict(DEFAULT_FEES)
    if path and path.exists():
        raw: dict = json.loads(path.read_text(encoding="utf-8"))
        for venue_raw, cfg in raw.items():
            venue = normalize_venue_name(venue_raw)
            schedule[venue] = FeeConfig(
                taker_bps=float(cfg.get("taker_bps", 5.0)),
                maker_bps=float(cfg.get("maker_bps", 2.0)),
            )
        print(f"[fees] loaded {len(raw)} venue override(s) from {path}", file=sys.stderr)
    return schedule


def _get_fee(schedule: dict[str, FeeConfig], venue: str, fallback_bps: float) -> FeeConfig:
    return schedule.get(normalize_venue_name(venue), FeeConfig(taker_bps=fallback_bps))


# ═══════════════════════════════════════════════════════════════════════════
# Simulation data structures
# ═══════════════════════════════════════════════════════════════════════════

@dataclass
class NetSimResult:
    """Top-of-book fill outcome with fees applied and capital impact recorded."""
    latency_ns: int
    reached_before_close: bool
    arb_valid_at_exec: bool
    filled_qty: float = 0.0
    buy_avg: float = 0.0
    sell_avg: float = 0.0
    gross_pnl_total: float = 0.0
    buy_fee: float = 0.0
    sell_fee: float = 0.0
    net_pnl_total: float = 0.0
    capital_used: float = 0.0
    note: str = ""

    @property
    def fee_total(self) -> float:
        return self.buy_fee + self.sell_fee


@dataclass
class MultiArbWindow:
    """ArbWindow with one NetSimResult per latency scenario."""
    pair_id: str
    buy_venue: str
    sell_venue: str
    symbol: str
    ts_open_ns: int
    open_buy_ask: float
    open_sell_bid: float
    open_spread_abs: float
    open_spread_bps: float
    peak_spread_abs: float
    peak_spread_bps: float
    ts_close_ns: int = 0
    results: dict[int, NetSimResult] = field(default_factory=dict)  # latency_ns → result

    @property
    def is_open(self) -> bool:
        return self.ts_close_ns == 0

    @property
    def ttl_ns(self) -> int:
        return self.ts_close_ns - self.ts_open_ns if self.ts_close_ns > 0 else -1

    @property
    def ttl_ms(self) -> float:
        t = self.ttl_ns
        return t / NS_PER_MS if t >= 0 else -1.0


@dataclass
class PendingExecution:
    """Order pair dispatched at window detection, arriving at execute_ts_ns."""
    execute_ts_ns: int
    latency_ns: int
    window: MultiArbWindow


@dataclass
class EquityPoint:
    """One row in the equity curve — recorded at every successful fill."""
    ts_ns: int
    pair_id: str
    filled_qty: float
    net_pnl: float
    quote: float   # quote balance after this fill
    base: float    # base balance after this fill (unchanged by arb)


@dataclass
class CapitalState:
    """
    Dual-currency capital tracker for one latency scenario.

    Cross-venue arb requires the QUOTE asset on the buy exchange (to pay for
    the base) and the BASE asset on the sell exchange (to deliver on the sell).
    After each round-trip:
      • quote changes by net_pnl   (bought base cheap, sold base dear minus fees)
      • base  is neutral            (bought qty on buy side, sold qty on sell side)
    """
    initial_quote: float   # quote-asset balance (e.g. USDT, USD, …)
    initial_base: float    # base-asset balance  (e.g. BTC, ETH, …)
    quote: float = 0.0
    base: float = 0.0
    peak_trade_quote: float = 0.0   # largest single buy-leg notional seen
    total_fills: int = 0
    equity_path: list = field(default_factory=list)   # list[EquityPoint]

    def __post_init__(self) -> None:
        self.quote = self.initial_quote
        self.base  = self.initial_base

    def record_fill(
        self,
        net_pnl_quote: float,
        buy_notional: float,
        ts_ns: int = 0,
        pair_id: str = "",
        filled_qty: float = 0.0,
    ) -> None:
        # base is neutral: qty bought on buy-venue equals qty sold on sell-venue
        self.quote += net_pnl_quote
        self.total_fills += 1
        if buy_notional > self.peak_trade_quote:
            self.peak_trade_quote = buy_notional
        self.equity_path.append(
            EquityPoint(
                ts_ns=ts_ns,
                pair_id=pair_id,
                filled_qty=filled_qty,
                net_pnl=net_pnl_quote,
                quote=self.quote,
                base=self.base,
            )
        )

    @property
    def quote_return_pct(self) -> float:
        if self.initial_quote <= 0.0:
            return 0.0
        return (self.quote - self.initial_quote) / self.initial_quote * 100.0


# ═══════════════════════════════════════════════════════════════════════════
# Net simulation (top-of-book, fees, capital constraint)
# ═══════════════════════════════════════════════════════════════════════════

_INF_TTL = 1 << 60  # sentinel for windows still open when exec fires


def _run_net_sim(
    buy_book: Optional[BookState],
    sell_book: Optional[BookState],
    latency_ns: int,
    ttl_ns: int,
    target_qty: float,
    fee_schedule: dict[str, FeeConfig],
    fallback_taker_bps: float,
    capital: CapitalState,
    exec_ts_ns: int = 0,
    pair_id: str = "",
    risk_pct: float = 0.0,
) -> NetSimResult:
    """
    Simulate one taker fill on both legs at top-of-book prices.

    ttl_ns   – use _INF_TTL when the window is still open at exec time
    capital  – mutated in-place on a successful fill (equity path appended)
    risk_pct – if > 0 and initial_quote > 0, cap fill at risk_pct% of current
               quote balance (position sizing).  Applied before capital floor.
    """
    if latency_ns >= ttl_ns:
        return NetSimResult(
            latency_ns=latency_ns,
            reached_before_close=False,
            arb_valid_at_exec=False,
            note=f"expired – latency {ns_to_us(latency_ns):.0f}µs > TTL {ns_to_ms(ttl_ns):.1f}ms",
        )

    if buy_book is None or sell_book is None:
        return NetSimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note="missing book data at execution time",
        )

    if not buy_book.asks or not sell_book.bids:
        return NetSimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note="empty book at execution time",
        )

    buy_px,  buy_liq  = buy_book.asks[0]
    sell_px, sell_liq = sell_book.bids[0]

    if sell_px <= buy_px:
        return NetSimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note="cross closed before order arrived",
        )

    # Fill quantity: level-0 liquidity cap, then target_qty cap
    qty = min(buy_liq, sell_liq)
    if target_qty > 0.0:
        qty = min(qty, target_qty)

    # Risk-pct sizing: cap at N% of current quote balance per trade.
    # Only active when initial_quote is set (otherwise there's no balance to fraction).
    if risk_pct > 0.0 and capital.initial_quote > 0.0 and buy_px > 0.0:
        qty = min(qty, (capital.quote * risk_pct / 100.0) / buy_px)

    # Dual-currency capital constraints:
    #   buy  leg needs QUOTE  →  max_qty = quote_balance / buy_px
    #   sell leg needs BASE   →  max_qty = base_balance
    constrained = capital.initial_quote > 0.0 or capital.initial_base > 0.0
    if constrained:
        max_from_quote = (capital.quote / buy_px) if capital.initial_quote > 0.0 else float("inf")
        max_from_base  = capital.base              if capital.initial_base  > 0.0 else float("inf")
        affordable = min(max_from_quote, max_from_base)
        if affordable <= 0.0:
            side = "quote" if max_from_quote <= 0.0 else "base"
            return NetSimResult(
                latency_ns=latency_ns,
                reached_before_close=True,
                arb_valid_at_exec=False,
                note=f"insufficient {side} capital",
            )
        qty = min(qty, affordable)

    if qty <= 0.0:
        return NetSimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note="qty reduced to zero by risk_pct or capital cap",
        )

    # Taker fees on both legs
    buy_fee_cfg  = _get_fee(fee_schedule, buy_book.venue,  fallback_taker_bps)
    sell_fee_cfg = _get_fee(fee_schedule, sell_book.venue, fallback_taker_bps)
    buy_fee  = buy_px  * qty * buy_fee_cfg.taker_rate
    sell_fee = sell_px * qty * sell_fee_cfg.taker_rate

    gross_pnl    = (sell_px - buy_px) * qty
    net_pnl      = gross_pnl - buy_fee - sell_fee
    buy_notional = buy_px * qty

    if net_pnl <= 0.0:
        return NetSimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note=f"fees exceed spread – net {net_pnl:.6f} on gross {gross_pnl:.6f}",
        )

    capital.record_fill(net_pnl, buy_notional, exec_ts_ns, pair_id, qty)

    return NetSimResult(
        latency_ns=latency_ns,
        reached_before_close=True,
        arb_valid_at_exec=True,
        filled_qty=qty,
        buy_avg=buy_px,
        sell_avg=sell_px,
        gross_pnl_total=gross_pnl,
        buy_fee=buy_fee,
        sell_fee=sell_fee,
        net_pnl_total=net_pnl,
        capital_used=buy_notional,
    )


# ═══════════════════════════════════════════════════════════════════════════
# Reporting
# ═══════════════════════════════════════════════════════════════════════════

_RESET  = "\033[0m"
_GREEN  = "\033[92m"
_YELLOW = "\033[93m"
_RED    = "\033[91m"
_BOLD   = "\033[1m"


def _c(text: str, code: str, use: bool) -> str:
    return f"{code}{text}{_RESET}" if use else text


def print_comparison(
    windows: list[MultiArbWindow],
    latency_ns_list: list[int],
    capital_states: dict[int, CapitalState],
    fee_schedule: dict[str, FeeConfig],
    use_color: bool,
) -> None:
    total = len(windows)
    W = 90
    sep = "═" * W

    print(f"\n{sep}")
    print(f"  MULTI-LATENCY SIMULATION  –  NET P&L (taker fees deducted)   "
          f"windows={total:,}")
    print(sep)

    if total == 0:
        print("  No arbitrage windows recorded.")
        print(sep)
        return

    # Header
    has_quote = any(capital_states[n].initial_quote > 0.0 for n in latency_ns_list)
    has_base  = any(capital_states[n].initial_base  > 0.0 for n in latency_ns_list)
    cap_header = ""
    if has_quote:
        cap_header += f"  {'Quote bal':>12}"
    if has_base:
        cap_header += f"  {'Base bal':>12}"
    print(
        f"  {'Latency':>9}  {'Reached':>8}  {'Viable':>8}  {'Fill%':>6}  "
        f"{'Gross P&L':>12}  {'Fees':>10}  {'Net P&L':>12}"
        + cap_header
    )
    print("  " + "─" * (W - 2))

    for lat_ns in latency_ns_list:
        lat_us = ns_to_us(lat_ns)
        lat_lbl = f"{lat_us:,.0f} µs" if lat_us < 1000 else f"{lat_us/1000:.1f} ms"

        rows = [w.results.get(lat_ns) for w in windows]
        reached   = [r for r in rows if r and r.reached_before_close]
        viable    = [r for r in reached if r.arb_valid_at_exec and r.filled_qty > 0]

        reach_pct = 100.0 * len(reached) / total if total else 0.0
        viable_pct = 100.0 * len(viable) / len(reached) if reached else 0.0
        fill_pct   = 100.0 * len(viable) / total if total else 0.0

        gross = sum(r.gross_pnl_total for r in viable)
        fees  = sum(r.fee_total        for r in viable)
        net   = sum(r.net_pnl_total    for r in viable)

        cap       = capital_states[lat_ns]
        net_col   = _c(f"{net:>+12.4f}", _GREEN if net >= 0 else _RED, use_color)
        gross_col = _c(f"{gross:>+12.4f}", _GREEN if gross >= 0 else _RED, use_color)

        cap_cols = ""
        if has_quote:
            cap_cols += f"  {cap.quote:>12,.4f}"
        if has_base:
            cap_cols += f"  {cap.base:>12,.6f}"

        print(
            f"  {lat_lbl:>9}  {reach_pct:>7.1f}%  {viable_pct:>7.1f}%  "
            f"{fill_pct:>5.1f}%  "
            f"{gross_col}  {-fees:>+10.4f}  {net_col}"
            + cap_cols
        )

    # Per-pair breakdown
    print(f"\n  Per-pair breakdown  (viable = net_pnl > 0 fill at given latency)")
    print("  " + "─" * (W - 2))

    pairs = sorted({w.pair_id for w in windows})
    for pair_id in pairs:
        pair_wins = [w for w in windows if w.pair_id == pair_id]
        row_parts = [f"  {pair_id:<24}  windows={len(pair_wins):5,}"]
        for lat_ns in latency_ns_list:
            lat_us = ns_to_us(lat_ns)
            lat_lbl = f"{lat_us:.0f}µs"
            viable = [
                w.results[lat_ns] for w in pair_wins
                if lat_ns in w.results
                and w.results[lat_ns].arb_valid_at_exec
                and w.results[lat_ns].net_pnl_total > 0
            ]
            net = sum(r.net_pnl_total for r in viable)
            row_parts.append(f"  {lat_lbl}: {len(viable):4,} fills / ${net:+.2f}")
        print("".join(row_parts))

    # Fee schedule in use
    print(f"\n  Fee schedule (taker bps):")
    for venue, cfg in sorted(fee_schedule.items()):
        print(f"    {venue:<12} taker={cfg.taker_bps:.1f} bps  maker={cfg.maker_bps:.1f} bps")

    print(sep)


def write_scenario_csv(
    windows: list[MultiArbWindow],
    lat_ns: int,
    out_path: Path,
) -> None:
    with out_path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow([
            "pair_id", "buy_venue", "sell_venue", "symbol",
            "ts_open_ns", "ts_open_dt", "ts_close_ns", "ttl_ms",
            "open_buy_ask", "open_sell_bid",
            "open_spread_abs", "open_spread_bps",
            "peak_spread_abs", "peak_spread_bps",
            "latency_us",
            "reached", "viable",
            "filled_qty", "buy_avg", "sell_avg",
            "gross_pnl", "buy_fee", "sell_fee", "net_pnl",
            "capital_used", "note",
        ])
        for win in windows:
            r = win.results.get(lat_ns)
            w.writerow([
                win.pair_id, win.buy_venue, win.sell_venue, win.symbol,
                win.ts_open_ns, ns_to_dt(win.ts_open_ns), win.ts_close_ns,
                f"{win.ttl_ms:.4f}" if win.ttl_ms >= 0 else "",
                win.open_buy_ask, win.open_sell_bid,
                win.open_spread_abs, f"{win.open_spread_bps:.4f}",
                win.peak_spread_abs, f"{win.peak_spread_bps:.4f}",
                f"{ns_to_us(lat_ns):.0f}",
                r.reached_before_close  if r else "",
                r.arb_valid_at_exec     if r else "",
                f"{r.filled_qty:.6f}"   if r else "",
                f"{r.buy_avg:.4f}"      if r else "",
                f"{r.sell_avg:.4f}"     if r else "",
                f"{r.gross_pnl_total:.6f}" if r else "",
                f"{r.buy_fee:.6f}"      if r else "",
                f"{r.sell_fee:.6f}"     if r else "",
                f"{r.net_pnl_total:.6f}"   if r else "",
                f"{r.capital_used:.4f}" if r else "",
                r.note                  if r else "",
            ])
    print(f"[csv] {len(windows):,} rows → {out_path}", file=sys.stderr)


def write_equity_csv(capital: CapitalState, out_path: Path) -> None:
    """Write the fill-by-fill equity curve for one latency scenario."""
    with out_path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow([
            "ts_ns", "ts_dt", "pair_id",
            "filled_qty", "net_pnl", "cumulative_net_pnl",
            "quote_balance", "base_balance",
        ])
        cumulative = 0.0
        for pt in capital.equity_path:
            cumulative += pt.net_pnl
            w.writerow([
                pt.ts_ns, ns_to_dt(pt.ts_ns), pt.pair_id,
                f"{pt.filled_qty:.6f}",
                f"{pt.net_pnl:.6f}",
                f"{cumulative:.6f}",
                f"{pt.quote:.6f}",
                f"{pt.base:.8f}",
            ])
    print(
        f"[equity] {len(capital.equity_path):,} fills → {out_path}",
        file=sys.stderr,
    )


# ═══════════════════════════════════════════════════════════════════════════
# Main pipeline
# ═══════════════════════════════════════════════════════════════════════════

def run(args: argparse.Namespace) -> int:
    latency_ns_list = sorted(int(us * NS_PER_US) for us in args.latencies_us)
    fee_schedule    = load_fee_schedule(args.fee_config)
    use_color       = sys.stdout.isatty() and not args.no_color

    # Announce fee schedule
    print("[fees] taker rates in use:", file=sys.stderr)
    for lat_ns in latency_ns_list:
        pass  # just validating
    for v, cfg in sorted(fee_schedule.items()):
        print(f"  {v:<12} taker={cfg.taker_bps:.1f} bps", file=sys.stderr)
    print(f"[sim]  latencies: {[f'{ns_to_us(n):.0f}µs' for n in latency_ns_list]}", file=sys.stderr)

    # Capital state per latency scenario
    capital_states: dict[int, CapitalState] = {
        ns: CapitalState(initial_quote=args.initial_quote, initial_base=args.initial_base)
        for ns in latency_ns_list
    }

    stream = TimeMergedBookStream(
        input_root=args.input,
        symbol_filter=args.symbol,
        venues=args.venues,
        buffer_size=args.buffer_size,
        max_depth=args.book_depth,
    )
    uob = UnifiedOrderbook(max_age_ms=args.max_book_age_ms)

    active_arbs: dict[str, MultiArbWindow] = {}

    # Min-heap of pending executions: (execute_ts_ns, tiebreak_seq, PendingExecution)
    # Using a heap ensures O(log n) fire at the correct stream position.
    pending_heap: list[tuple[int, int, PendingExecution]] = []
    _seq = 0

    all_windows: list[MultiArbWindow] = []
    events_processed = 0
    last_ts = 0

    try:
        for venue, book in stream:
            current_ts = book.ts_ns
            last_ts    = current_ts
            events_processed += 1

            # ── 1. Fire pending execs whose arrival time has passed ────────
            while pending_heap and pending_heap[0][0] <= current_ts:
                _, _, pe = heapq.heappop(pending_heap)
                w = pe.window
                if pe.latency_ns not in w.results:
                    buy_b  = uob.get(w.buy_venue)
                    sell_b = uob.get(w.sell_venue)
                    # Window may already be closed; use actual TTL or _INF_TTL if still open
                    ttl = (w.ts_close_ns - w.ts_open_ns) if w.ts_close_ns > 0 else _INF_TTL
                    cap = capital_states[pe.latency_ns]
                    w.results[pe.latency_ns] = _run_net_sim(
                        buy_b, sell_b, pe.latency_ns, ttl,
                        args.target_qty, fee_schedule, args.default_taker_bps, cap,
                        exec_ts_ns=pe.execute_ts_ns, pair_id=w.pair_id,
                        risk_pct=args.risk_pct,
                    )

            # ── 2. Update unified orderbook ────────────────────────────────
            uob.update(venue, book)

            # ── 3. Periodic staleness eviction ────────────────────────────
            if events_processed % 100_000 == 0:
                uob.evict_stale(current_ts)

            if uob.venue_count < 2:
                continue

            books   = uob.snapshot()
            crosses = detect_crosses(books, args.min_spread_bps, args.max_book_age_diff_ms)
            live_ids = {f"{bv}→{sv}" for bv, sv, *_ in crosses}

            # ── 4. Open new arb windows ────────────────────────────────────
            for buy_v, sell_v, buy_ask, sell_bid, sp_abs, sp_bps in crosses:
                pair_id = f"{buy_v}→{sell_v}"
                if pair_id not in active_arbs:
                    w = MultiArbWindow(
                        pair_id=pair_id,
                        buy_venue=buy_v,
                        sell_venue=sell_v,
                        symbol=book.symbol,
                        ts_open_ns=current_ts,
                        open_buy_ask=buy_ask,
                        open_sell_bid=sell_bid,
                        open_spread_abs=sp_abs,
                        open_spread_bps=sp_bps,
                        peak_spread_abs=sp_abs,
                        peak_spread_bps=sp_bps,
                    )
                    active_arbs[pair_id] = w
                    # Schedule one pending execution per latency scenario
                    for lat_ns in latency_ns_list:
                        pe = PendingExecution(
                            execute_ts_ns=current_ts + lat_ns,
                            latency_ns=lat_ns,
                            window=w,
                        )
                        _seq += 1
                        heapq.heappush(pending_heap, (pe.execute_ts_ns, _seq, pe))
                else:
                    w = active_arbs[pair_id]
                    if sp_abs > w.peak_spread_abs:
                        w.peak_spread_abs = sp_abs
                        w.peak_spread_bps = sp_bps

            # ── 5. Close windows that are no longer live ───────────────────
            for pair_id in list(active_arbs):
                if pair_id not in live_ids:
                    w = active_arbs.pop(pair_id)
                    w.ts_close_ns = current_ts
                    ttl_ms = (current_ts - w.ts_open_ns) / NS_PER_MS
                    if args.ttl_min_ms <= 0 or ttl_ms >= args.ttl_min_ms:
                        all_windows.append(w)

            # ── 6. Progress heartbeat ─────────────────────────────────────
            if args.verbose and events_processed % 500_000 == 0:
                print(
                    f"[progress] events={events_processed:>10,}  "
                    f"active={len(active_arbs):3d}  "
                    f"closed={len(all_windows):,}  "
                    f"heap={len(pending_heap):,}  "
                    f"ts={ns_to_dt(current_ts)}",
                    file=sys.stderr,
                )

    finally:
        stream.close()

    # ── Drain any pending execs that didn't fire during the stream ─────────
    while pending_heap:
        _, _, pe = heapq.heappop(pending_heap)
        w = pe.window
        if pe.latency_ns not in w.results:
            buy_b  = uob.get(w.buy_venue)
            sell_b = uob.get(w.sell_venue)
            ttl = (w.ts_close_ns - w.ts_open_ns) if w.ts_close_ns > 0 else (last_ts - w.ts_open_ns)
            cap = capital_states[pe.latency_ns]
            w.results[pe.latency_ns] = _run_net_sim(
                buy_b, sell_b, pe.latency_ns, ttl,
                args.target_qty, fee_schedule, args.default_taker_bps, cap,
                exec_ts_ns=pe.execute_ts_ns, pair_id=w.pair_id,
                risk_pct=args.risk_pct,
            )

    # Close any still-open windows (cross active until EOF)
    for w in active_arbs.values():
        w.ts_close_ns = 0
        if args.ttl_min_ms <= 0:
            all_windows.append(w)

    print_comparison(all_windows, latency_ns_list, capital_states, fee_schedule, use_color)

    if args.output_dir:
        out_dir = Path(args.output_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        for lat_ns in latency_ns_list:
            lat_us = int(ns_to_us(lat_ns))
            write_scenario_csv(all_windows, lat_ns, out_dir / f"sim_{lat_us}us.csv")
            write_equity_csv(capital_states[lat_ns], out_dir / f"equity_{lat_us}us.csv")

    print(
        f"\n[done] events_processed={events_processed:,}  "
        f"windows_recorded={len(all_windows):,}",
        file=sys.stderr,
    )
    return 0


# ═══════════════════════════════════════════════════════════════════════════
# CLI
# ═══════════════════════════════════════════════════════════════════════════

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # ── Input ────────────────────────────────────────────────────────────
    p.add_argument("--input", required=True, type=Path, metavar="DIR",
                   help="Run folder containing raw venue .jsonl / .jsonl.gz files.")
    p.add_argument("--symbol", default=None, metavar="SYM",
                   help="Canonical symbol filter, e.g. BTCUSDT.")
    p.add_argument("--venues", nargs="*", default=None, metavar="V",
                   help="Restrict to a venue subset, e.g. --venues binance bybit.")
    p.add_argument("--buffer-size", type=int, default=256, metavar="N",
                   help="Book_state rows to prefetch per venue stream.  Default: 256.")

    # ── Orderbook ────────────────────────────────────────────────────────
    p.add_argument("--book-depth", type=int, default=10, metavar="N",
                   help="Price levels to retain per side.  Default: 10.")
    p.add_argument("--max-book-age-ms", type=float, default=10_000.0, metavar="MS",
                   help="Evict venue book if no update for this many ms.  Default: 10000.")
    p.add_argument("--max-book-age-diff-ms", type=float, default=5_000.0, metavar="MS",
                   help="Skip pair comparison if books differ in age by this much.  Default: 5000.")

    # ── Detection filters ────────────────────────────────────────────────
    p.add_argument("--min-spread-bps", type=float, default=0.0, metavar="BPS",
                   help="Minimum spread (bps) to record a window.  Default: 0.")
    p.add_argument("--ttl-min-ms", type=float, default=0.0, metavar="MS",
                   help="Only record windows lasting at least this many ms.  Default: 0.")

    # ── Simulation ───────────────────────────────────────────────────────
    p.add_argument(
        "--latencies-us", nargs="+", type=float, default=[500.0], metavar="US",
        help="One or more one-way latency values in microseconds.  Default: 500.",
    )
    p.add_argument("--target-qty", type=float, default=0.0, metavar="QTY",
                   help="Max fill per leg in base units.  0 = use full level-0 qty.  Default: 0.")
    p.add_argument("--initial-quote", type=float, default=0.0, metavar="AMT",
                   help=(
                       "Starting quote-asset balance (e.g. USDT) for the buy leg.  "
                       "0 = unconstrained.  Default: 0."
                   ))
    p.add_argument("--initial-base", type=float, default=0.0, metavar="AMT",
                   help=(
                       "Starting base-asset balance (e.g. BTC) for the sell leg.  "
                       "0 = unconstrained.  Default: 0."
                   ))
    p.add_argument("--risk-pct", type=float, default=0.0, metavar="PCT",
                   help=(
                       "Size each trade as PCT%% of current quote balance.  "
                       "E.g. 5 = use at most 5%% of remaining quote per trade.  "
                       "Requires --initial-quote.  0 = disabled.  Default: 0."
                   ))

    # ── Fees ─────────────────────────────────────────────────────────────
    p.add_argument("--fee-config", type=Path, default=None, metavar="FILE",
                   help="JSON file with per-venue taker/maker fees (bps).  Default: built-in rates.")
    p.add_argument("--default-taker-bps", type=float, default=5.0, metavar="BPS",
                   help="Fallback taker fee (bps) for unlisted venues.  Default: 5.0.")

    # ── Output ───────────────────────────────────────────────────────────
    p.add_argument("--output-dir", default=None, metavar="DIR",
                   help="Directory to write one CSV per latency scenario.")
    p.add_argument("--verbose", action="store_true",
                   help="Print progress to stderr every 500k events.")
    p.add_argument("--no-color", action="store_true",
                   help="Disable ANSI colour codes.")

    return p


def main() -> int:
    parser = build_arg_parser()
    args   = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
