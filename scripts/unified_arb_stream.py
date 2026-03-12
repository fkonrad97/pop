#!/usr/bin/env python3
"""
unified_arb_stream.py – Cross-venue arbitrage analysis on persisted book_state data.

Streams all venue JSONL files in global timestamp order (k-way heap merge),
maintains a unified per-venue orderbook, detects raw bid-ask crosses between
venues, and simulates execution paths with configurable one-way latency and TTL.

All spreads are GROSS – no fees, rebates, or transaction costs are modelled.

Usage examples
--------------
# Raw detection, no simulation filter:
    python unified_arb_stream.py \\
        --input /persist/pop-data/2026-03-05-20-27-39 \\
        --symbol BTCUSDT

# With 500 µs one-way latency, print only windows ≥ 2 bps:
    python unified_arb_stream.py \\
        --input /persist/pop-data/2026-03-05-20-27-39 \\
        --symbol BTCUSDT --latency-us 500 --min-spread-bps 2 --show-arbs

# Full simulation + CSV dump:
    python unified_arb_stream.py \\
        --input /persist/pop-data/2026-03-05-20-27-39 \\
        --symbol BTCUSDT --latency-us 1000 --ttl-min-ms 10 \\
        --output arb_results.csv --show-arbs
"""

from __future__ import annotations

import argparse
import csv
import heapq
import os
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator, Optional

# ── Resolve loader from the same scripts/ directory ─────────────────────────
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from jsonl_stream_loader import (
    RawBookStateStream,
    discover_inputs,
    normalize_symbol,
    normalize_venue_name,
    venue_from_path,
)

# ── Time unit constants ──────────────────────────────────────────────────────
NS_PER_US: int = 1_000
NS_PER_MS: int = 1_000_000
NS_PER_S: int = 1_000_000_000


def ns_to_dt(ns: int) -> str:
    """Nanosecond epoch → UTC datetime string (millisecond precision)."""
    dt = datetime.fromtimestamp(ns / NS_PER_S, tz=timezone.utc)
    return dt.strftime("%Y-%m-%d %H:%M:%S.%f")[:-3]


def ns_to_us(ns: int) -> float:
    return ns / NS_PER_US


def ns_to_ms(ns: int) -> float:
    return ns / NS_PER_MS


# ═══════════════════════════════════════════════════════════════════════════
# Data structures
# ═══════════════════════════════════════════════════════════════════════════

@dataclass
class BookState:
    """Snapshot of one venue's best N levels at a point in time."""
    venue: str
    symbol: str
    ts_ns: int
    bids: list[tuple[float, float]]  # [(price, qty), ...] descending
    asks: list[tuple[float, float]]  # [(price, qty), ...] ascending

    @property
    def best_bid(self) -> float:
        return self.bids[0][0] if self.bids else 0.0

    @property
    def best_ask(self) -> float:
        return self.asks[0][0] if self.asks else float("inf")

    @property
    def mid(self) -> float:
        b, a = self.best_bid, self.best_ask
        if b > 0.0 and a < float("inf"):
            return (b + a) / 2.0
        return 0.0

    @classmethod
    def from_row(cls, row: dict, max_depth: int) -> "BookState":
        bids = [(float(lv["price"]), float(lv["quantity"])) for lv in row["bids"][:max_depth]]
        asks = [(float(lv["price"]), float(lv["quantity"])) for lv in row["asks"][:max_depth]]
        return cls(
            venue=row["venue"],
            symbol=row["canonical_symbol"],
            ts_ns=row["ts_book_ns"],
            bids=bids,
            asks=asks,
        )


@dataclass
class ArbWindow:
    """A single continuous period where a cross-venue bid-ask cross existed."""
    pair_id: str            # e.g. "bybit→binance"
    buy_venue: str          # venue where we BUY (lift their ask)
    sell_venue: str         # venue where we SELL (hit their bid)
    symbol: str
    ts_open_ns: int         # when the cross first appeared
    open_buy_ask: float     # ask on buy_venue at detection
    open_sell_bid: float    # bid on sell_venue at detection
    open_spread_abs: float  # sell_bid - buy_ask at detection
    open_spread_bps: float  # same in basis points
    peak_spread_abs: float  # largest |spread| seen while window was open
    peak_spread_bps: float
    ts_close_ns: int = 0    # when the cross disappeared (0 = still open)
    sim_result: Optional["SimResult"] = None

    @property
    def is_open(self) -> bool:
        return self.ts_close_ns == 0

    @property
    def ttl_ns(self) -> int:
        if self.ts_close_ns > 0:
            return self.ts_close_ns - self.ts_open_ns
        return -1

    @property
    def ttl_ms(self) -> float:
        t = self.ttl_ns
        return t / NS_PER_MS if t >= 0 else -1.0


@dataclass
class SimResult:
    """Outcome of one simulated execution path."""
    latency_ns: int
    reached_before_close: bool   # order arrived before arb window closed
    arb_valid_at_exec: bool      # cross still existed when order arrived
    filled_qty: float = 0.0
    buy_avg: float = 0.0
    sell_avg: float = 0.0
    gross_pnl_per_unit: float = 0.0
    gross_pnl_total: float = 0.0
    note: str = ""


@dataclass
class PendingExecution:
    """Represents an order pair dispatched at arb detection, arriving at execute_ts_ns."""
    execute_ts_ns: int
    window: ArbWindow


# ═══════════════════════════════════════════════════════════════════════════
# Time-merged k-way stream
# ═══════════════════════════════════════════════════════════════════════════

class TimeMergedBookStream:
    """
    Merge per-venue book_state streams in globally ascending ts_book_ns order.

    Internally holds one "next row" per venue in a min-heap.  Memory footprint
    is O(num_venues × buffer_size) at any moment.
    """

    def __init__(
        self,
        input_root: Path,
        symbol_filter: str | None,
        venues: list[str] | None,
        buffer_size: int,
        max_depth: int,
    ) -> None:
        self._max_depth = max_depth
        self._streams: dict[str, RawBookStateStream] = {}
        # heap entries: (ts_ns, tiebreak_seq, venue_name, raw_row)
        self._heap: list[tuple[int, int, str, dict]] = []
        self._seq = 0

        for path in discover_inputs(input_root, venues):
            venue = venue_from_path(path)
            stream = RawBookStateStream(
                path=path,
                symbol_filter=symbol_filter,
                buffer_size=buffer_size,
            )
            self._streams[venue] = stream
            row = stream.next_row()
            if row is not None:
                heapq.heappush(self._heap, (row["ts_book_ns"], self._seq, venue, row))
                self._seq += 1

        print(
            f"[stream] loaded {len(self._streams)} venue(s): {sorted(self._streams)}",
            file=sys.stderr,
        )

    def __iter__(self) -> Iterator[tuple[str, BookState]]:
        while self._heap:
            ts, _, venue, raw = heapq.heappop(self._heap)
            yield venue, BookState.from_row(raw, self._max_depth)
            nxt = self._streams[venue].next_row()
            if nxt is not None:
                heapq.heappush(self._heap, (nxt["ts_book_ns"], self._seq, venue, nxt))
                self._seq += 1

    def close(self) -> None:
        for s in self._streams.values():
            s.close()


# ═══════════════════════════════════════════════════════════════════════════
# Unified orderbook
# ═══════════════════════════════════════════════════════════════════════════

class UnifiedOrderbook:
    """
    Maintains the latest BookState for every venue.

    Memory is naturally bounded: only the most recent book per venue is kept.
    An optional staleness eviction removes venues that have stopped updating.
    """

    def __init__(self, max_age_ms: float = 10_000.0) -> None:
        self._books: dict[str, BookState] = {}
        self._max_age_ns = int(max_age_ms * NS_PER_MS)

    def update(self, venue: str, book: BookState) -> None:
        self._books[venue] = book

    def get(self, venue: str) -> BookState | None:
        return self._books.get(venue)

    def snapshot(self) -> dict[str, BookState]:
        return dict(self._books)

    def evict_stale(self, now_ns: int) -> list[str]:
        stale = [v for v, b in self._books.items() if (now_ns - b.ts_ns) > self._max_age_ns]
        for v in stale:
            del self._books[v]
        return stale

    @property
    def venue_count(self) -> int:
        return len(self._books)


# ═══════════════════════════════════════════════════════════════════════════
# Arbitrage detection
# ═══════════════════════════════════════════════════════════════════════════

def detect_crosses(
    books: dict[str, BookState],
    min_spread_bps: float,
    max_age_diff_ms: float,
) -> list[tuple[str, str, float, float, float, float]]:
    """
    Scan all directed venue pairs for a bid-ask cross.

    A cross exists when:
        sell_venue.best_bid  >  buy_venue.best_ask

    Returns a list of tuples:
        (buy_venue, sell_venue, buy_ask, sell_bid, spread_abs, spread_bps)

    Pairs where either book is too stale relative to the other are skipped.
    """
    max_diff_ns = max_age_diff_ms * NS_PER_MS
    results: list[tuple[str, str, float, float, float, float]] = []
    venue_list = list(books)

    for buy_v in venue_list:
        for sell_v in venue_list:
            if buy_v == sell_v:
                continue
            buy_b = books[buy_v]
            sell_b = books[sell_v]

            # Skip if books are too far apart in time
            if abs(buy_b.ts_ns - sell_b.ts_ns) > max_diff_ns:
                continue

            if not buy_b.asks or not sell_b.bids:
                continue

            buy_ask = buy_b.asks[0][0]
            sell_bid = sell_b.bids[0][0]

            if sell_bid <= buy_ask:
                continue

            mid = (sell_bid + buy_ask) / 2.0
            spread_abs = sell_bid - buy_ask
            spread_bps = (spread_abs / mid) * 10_000.0

            if spread_bps < min_spread_bps:
                continue

            results.append((buy_v, sell_v, buy_ask, sell_bid, spread_abs, spread_bps))

    return results


# ═══════════════════════════════════════════════════════════════════════════
# Execution simulation
# ═══════════════════════════════════════════════════════════════════════════

def run_simulation(
    buy_book: BookState | None,
    sell_book: BookState | None,
    latency_ns: int,
    ttl_ns: int,
    target_qty: float = 0.0,
) -> SimResult:
    """
    Simulate dispatching orders at window detection, arriving latency_ns later.

    Execution is evaluated at the TOP OF THE BOOK only (level 0 bid/ask prices).
    Fill quantity = min(best_ask_qty, best_bid_qty), optionally capped by
    target_qty when target_qty > 0.

    ttl_ns       – duration of the arb window; latency_ns >= ttl_ns means the
                   orders expire before reaching the exchange.
    target_qty   – maximum BTC per leg (0 = no cap, use full level-0 qty).
    """
    reached = latency_ns < ttl_ns
    if not reached:
        return SimResult(
            latency_ns=latency_ns,
            reached_before_close=False,
            arb_valid_at_exec=False,
            note=f"expired – latency {ns_to_us(latency_ns):.0f}µs > TTL {ns_to_ms(ttl_ns):.1f}ms",
        )

    if buy_book is None or sell_book is None:
        return SimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note="missing book data at execution time",
        )

    if not buy_book.asks or not sell_book.bids:
        return SimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note="empty book at execution time",
        )

    # Top-of-book prices
    buy_ask_px,  buy_ask_qty  = buy_book.asks[0]
    sell_bid_px, sell_bid_qty = sell_book.bids[0]

    if sell_bid_px <= buy_ask_px:
        return SimResult(
            latency_ns=latency_ns,
            reached_before_close=True,
            arb_valid_at_exec=False,
            note="cross closed before order arrived at exchange",
        )

    # Fillable qty at top-of-book, capped by target_qty if set
    filled = min(buy_ask_qty, sell_bid_qty)
    if target_qty > 0.0:
        filled = min(filled, target_qty)

    pnl_unit = sell_bid_px - buy_ask_px
    return SimResult(
        latency_ns=latency_ns,
        reached_before_close=True,
        arb_valid_at_exec=True,
        filled_qty=filled,
        buy_avg=buy_ask_px,
        sell_avg=sell_bid_px,
        gross_pnl_per_unit=pnl_unit,
        gross_pnl_total=filled * pnl_unit,
    )


# ═══════════════════════════════════════════════════════════════════════════
# Reporting helpers
# ═══════════════════════════════════════════════════════════════════════════

_RESET  = "\033[0m"
_GREEN  = "\033[92m"
_YELLOW = "\033[93m"
_RED    = "\033[91m"
_CYAN   = "\033[96m"
_BOLD   = "\033[1m"


def _color(text: str, code: str, use_color: bool) -> str:
    return f"{code}{text}{_RESET}" if use_color else text


def fmt_window(w: ArbWindow, use_color: bool = True) -> str:
    dt = ns_to_dt(w.ts_open_ns)
    direction = f"{w.buy_venue.upper():<8s}→{w.sell_venue.upper():<8s}"
    spread_str = f"+{w.peak_spread_bps:6.2f} bps  (+${w.peak_spread_abs:.4f})"
    ttl_str = f"{w.ttl_ms:8.1f} ms" if w.ttl_ms >= 0 else "   open   "

    parts = [
        f"[ARB] {dt}",
        _color(f" {w.symbol} ", _CYAN, use_color),
        f" {direction}",
        f" spread={_color(spread_str, _GREEN, use_color)}",
        f" TTL={ttl_str}",
    ]

    if w.sim_result is not None:
        r = w.sim_result
        if r.arb_valid_at_exec and r.filled_qty > 0:
            sim_str = (
                f"  SIM ✓  filled={r.filled_qty:.5f}  "
                f"buy@{r.buy_avg:.2f}  sell@{r.sell_avg:.2f}  "
                f"gross={_color(f'{r.gross_pnl_total:+.4f} USD', _GREEN, use_color)}"
            )
        elif r.reached_before_close:
            sim_str = f"  SIM ✗  arrived but {_color(r.note, _YELLOW, use_color)}"
        else:
            sim_str = f"  SIM ✗  {_color(r.note, _RED, use_color)}"
        parts.append(sim_str)

    return "".join(parts)


def print_summary(windows: list[ArbWindow], latency_ns: int, use_color: bool) -> None:
    total = len(windows)
    if total == 0:
        print("\n[summary] No arbitrage windows detected.")
        return

    simulated   = [w for w in windows if w.sim_result is not None]
    reached     = [w for w in simulated if w.sim_result.reached_before_close]
    filled      = [w for w in reached   if w.sim_result.arb_valid_at_exec and w.sim_result.filled_qty > 0]

    spreads_bps  = [w.peak_spread_bps for w in windows]
    spreads_abs  = [w.peak_spread_abs for w in windows]
    ttls_ms      = [w.ttl_ms          for w in windows if w.ttl_ms >= 0]

    total_gross_pnl = sum(w.sim_result.gross_pnl_total for w in filled)

    W = 72
    sep = "═" * W
    print(f"\n{sep}")
    print(f"  ARBITRAGE SUMMARY  –  GROSS (no fees)")
    print(sep)
    print(f"  {'Total windows detected':<35s}: {total:>10,}")
    print(f"  {'Orders simulated':<35s}: {len(simulated):>10,}")
    print(f"  {'Reached exchange before close':<35s}: {len(reached):>10,}  ({100*len(reached)/total:.1f}%)")
    print(f"  {'Executable fills':<35s}: {len(filled):>10,}  ({100*len(filled)/total:.1f}%)")
    print(f"  {'Latency assumed (one-way)':<35s}: {ns_to_us(latency_ns):>10.0f} µs")
    print()
    if spreads_bps:
        print(f"  {'Peak spread (bps) – mean':<35s}: {sum(spreads_bps)/len(spreads_bps):>10.2f}")
        print(f"  {'Peak spread (bps) – median':<35s}: {sorted(spreads_bps)[len(spreads_bps)//2]:>10.2f}")
        print(f"  {'Peak spread (bps) – max':<35s}: {max(spreads_bps):>10.2f}")
        print(f"  {'Peak spread ($) – mean':<35s}: ${sum(spreads_abs)/len(spreads_abs):>9.4f}")
        print(f"  {'Peak spread ($) – max':<35s}: ${max(spreads_abs):>9.4f}")
    if ttls_ms:
        print()
        print(f"  {'TTL – mean':<35s}: {sum(ttls_ms)/len(ttls_ms):>10.1f} ms")
        print(f"  {'TTL – median':<35s}: {sorted(ttls_ms)[len(ttls_ms)//2]:>10.1f} ms")
        print(f"  {'TTL – max':<35s}: {max(ttls_ms):>10.1f} ms")
        print(f"  {'TTL – min':<35s}: {min(ttls_ms):>10.1f} ms")
    if filled:
        print()
        print(f"  {'Total gross P&L (all fills)':<35s}: ${total_gross_pnl:>+10.4f}")

    # Per-pair breakdown
    print(f"\n  Per-pair breakdown:")
    pair_counts = Counter(w.pair_id for w in windows)
    for pair_id, cnt in pair_counts.most_common():
        pair_filled = sum(1 for w in windows if w.pair_id == pair_id and w.sim_result and w.sim_result.arb_valid_at_exec)
        pair_pnl    = sum(w.sim_result.gross_pnl_total for w in windows
                          if w.pair_id == pair_id and w.sim_result and w.sim_result.gross_pnl_total > 0)
        ttl_mean    = [w.ttl_ms for w in windows if w.pair_id == pair_id and w.ttl_ms >= 0]
        ttl_str     = f"{sum(ttl_mean)/len(ttl_mean):.1f} ms avg TTL" if ttl_mean else ""
        print(f"    {pair_id:<28s}  {cnt:6,} windows  {pair_filled:5,} viable  "
              f"gross={pair_pnl:+.4f} USD  {ttl_str}")

    print(sep)


def write_csv(windows: list[ArbWindow], out_path: Path) -> None:
    with out_path.open("w", newline="", encoding="utf-8") as fh:
        w = csv.writer(fh)
        w.writerow([
            "pair_id", "buy_venue", "sell_venue", "symbol",
            "ts_open_ns", "ts_open_dt", "ts_close_ns", "ttl_ms",
            "open_buy_ask", "open_sell_bid",
            "open_spread_abs", "open_spread_bps",
            "peak_spread_abs", "peak_spread_bps",
            "sim_reached", "sim_valid",
            "sim_filled_qty", "sim_buy_avg", "sim_sell_avg",
            "sim_gross_pnl_unit", "sim_gross_pnl_total", "sim_note",
        ])
        for win in windows:
            r = win.sim_result
            w.writerow([
                win.pair_id, win.buy_venue, win.sell_venue, win.symbol,
                win.ts_open_ns, ns_to_dt(win.ts_open_ns), win.ts_close_ns,
                f"{win.ttl_ms:.4f}" if win.ttl_ms >= 0 else "",
                win.open_buy_ask, win.open_sell_bid,
                win.open_spread_abs, f"{win.open_spread_bps:.4f}",
                win.peak_spread_abs, f"{win.peak_spread_bps:.4f}",
                r.reached_before_close  if r else "",
                r.arb_valid_at_exec     if r else "",
                f"{r.filled_qty:.6f}"   if r else "",
                f"{r.buy_avg:.6f}"      if r else "",
                f"{r.sell_avg:.6f}"     if r else "",
                f"{r.gross_pnl_per_unit:.6f}" if r else "",
                f"{r.gross_pnl_total:.6f}"    if r else "",
                r.note                  if r else "",
            ])
    print(f"[csv] {len(windows):,} rows → {out_path}", file=sys.stderr)


# ═══════════════════════════════════════════════════════════════════════════
# Main pipeline
# ═══════════════════════════════════════════════════════════════════════════

def run(args: argparse.Namespace) -> int:
    latency_ns = int(args.latency_us * NS_PER_US)
    use_color  = sys.stdout.isatty() and not args.no_color

    stream = TimeMergedBookStream(
        input_root=args.input,
        symbol_filter=args.symbol,
        venues=args.venues,
        buffer_size=args.buffer_size,
        max_depth=args.book_depth,
    )
    uob = UnifiedOrderbook(max_age_ms=args.max_book_age_ms)

    # pair_id → open ArbWindow
    active_arbs: dict[str, ArbWindow] = {}

    # Orders dispatched but not yet arrived at the exchange
    pending_execs: list[PendingExecution] = []

    # Completed windows (closed + simulated)
    all_windows: list[ArbWindow] = []

    events_processed   = 0
    progress_interval  = 500_000
    evict_interval     = 100_000

    try:
        for venue, book in stream:
            current_ts = book.ts_ns
            events_processed += 1

            # ── 1. Fire pending simulations whose arrival time has passed ──
            still_pending: list[PendingExecution] = []
            for pe in pending_execs:
                if current_ts >= pe.execute_ts_ns:
                    w = pe.window
                    buy_b  = uob.get(w.buy_venue)
                    sell_b = uob.get(w.sell_venue)
                    ttl_ns = (
                        w.ts_close_ns - w.ts_open_ns
                        if not w.is_open
                        else current_ts - w.ts_open_ns
                    )
                    if w.sim_result is None:
                        w.sim_result = run_simulation(buy_b, sell_b, latency_ns, ttl_ns, args.target_qty)
                else:
                    still_pending.append(pe)
            pending_execs = still_pending

            # ── 2. Update unified orderbook ────────────────────────────────
            uob.update(venue, book)

            # ── 3. Periodic staleness eviction ────────────────────────────
            if events_processed % evict_interval == 0:
                uob.evict_stale(current_ts)

            # ── 4. Need at least 2 live venues to detect anything ─────────
            if uob.venue_count < 2:
                continue

            books   = uob.snapshot()
            crosses = detect_crosses(books, args.min_spread_bps, args.max_book_age_diff_ms)
            live_ids = {f"{bv}→{sv}" for bv, sv, *_ in crosses}

            # ── 5. Open new arb windows ────────────────────────────────────
            for buy_v, sell_v, buy_ask, sell_bid, sp_abs, sp_bps in crosses:
                pair_id = f"{buy_v}→{sell_v}"
                if pair_id not in active_arbs:
                    w = ArbWindow(
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
                    pending_execs.append(PendingExecution(
                        execute_ts_ns=current_ts + latency_ns,
                        window=w,
                    ))
                else:
                    # Extend peak spread if cross widened
                    w = active_arbs[pair_id]
                    if sp_abs > w.peak_spread_abs:
                        w.peak_spread_abs = sp_abs
                        w.peak_spread_bps = sp_bps

            # ── 6. Close arb windows that are no longer active ────────────
            for pair_id in list(active_arbs):
                if pair_id not in live_ids:
                    w = active_arbs.pop(pair_id)
                    w.ts_close_ns = current_ts

                    # If pending exec hasn't fired yet, resolve it now
                    if w.sim_result is None:
                        ttl_ns = current_ts - w.ts_open_ns
                        for pe in pending_execs:
                            if pe.window is w:
                                buy_b  = uob.get(w.buy_venue)
                                sell_b = uob.get(w.sell_venue)
                                w.sim_result = run_simulation(
                                    buy_b, sell_b, latency_ns, ttl_ns, args.target_qty
                                )
                                break
                        # If somehow still None (no pending exec), mark missed
                        if w.sim_result is None:
                            w.sim_result = SimResult(
                                latency_ns=latency_ns,
                                reached_before_close=latency_ns < (current_ts - w.ts_open_ns),
                                arb_valid_at_exec=False,
                                note="no pending execution found",
                            )

                    # Filter by TTL minimum
                    if args.ttl_min_ms <= 0 or w.ttl_ms >= args.ttl_min_ms:
                        all_windows.append(w)
                        if args.show_arbs:
                            print(fmt_window(w, use_color))

                    # Memory safety: flush if list grows too large
                    if len(all_windows) > args.max_windows:
                        _flush_excess(all_windows, args)

            # ── 7. Progress heartbeat ─────────────────────────────────────
            if args.verbose and events_processed % progress_interval == 0:
                print(
                    f"[progress] events={events_processed:>10,}  "
                    f"active_arbs={len(active_arbs):3d}  "
                    f"closed={len(all_windows):,}  "
                    f"ts={ns_to_dt(current_ts)}",
                    file=sys.stderr,
                )

    finally:
        stream.close()

    # Close any windows still open at end-of-data
    for w in active_arbs.values():
        w.ts_close_ns = 0  # mark as "never closed" (still open at EOF)
        if args.ttl_min_ms <= 0:
            all_windows.append(w)

    print_summary(all_windows, latency_ns, use_color)

    if args.output:
        write_csv(all_windows, Path(args.output))

    print(
        f"\n[done] events_processed={events_processed:,}  "
        f"windows_recorded={len(all_windows):,}",
        file=sys.stderr,
    )
    return 0


def _flush_excess(windows: list[ArbWindow], args: argparse.Namespace) -> None:
    """Emit oldest half to CSV/stdout and evict them to free memory."""
    cutoff = len(windows) // 2
    if args.output:
        out_path = Path(args.output)
        mode = "a" if out_path.exists() else "w"
        with out_path.open(mode, newline="", encoding="utf-8") as fh:
            w = csv.writer(fh)
            if mode == "w":
                w.writerow([
                    "pair_id", "buy_venue", "sell_venue", "symbol",
                    "ts_open_ns", "ts_open_dt", "ts_close_ns", "ttl_ms",
                    "open_spread_abs", "open_spread_bps",
                    "peak_spread_abs", "peak_spread_bps",
                    "sim_reached", "sim_valid", "sim_filled_qty",
                    "sim_gross_pnl_total", "sim_note",
                ])
            for win in windows[:cutoff]:
                r = win.sim_result
                w.writerow([
                    win.pair_id, win.buy_venue, win.sell_venue, win.symbol,
                    win.ts_open_ns, ns_to_dt(win.ts_open_ns), win.ts_close_ns,
                    f"{win.ttl_ms:.4f}" if win.ttl_ms >= 0 else "",
                    win.open_spread_abs, f"{win.open_spread_bps:.4f}",
                    win.peak_spread_abs, f"{win.peak_spread_bps:.4f}",
                    r.reached_before_close  if r else "",
                    r.arb_valid_at_exec     if r else "",
                    f"{r.filled_qty:.6f}"   if r else "",
                    f"{r.gross_pnl_total:.6f}" if r else "",
                    r.note                  if r else "",
                ])
        print(f"[flush] streamed {cutoff:,} windows to {args.output}", file=sys.stderr)

    del windows[:cutoff]


# ═══════════════════════════════════════════════════════════════════════════
# CLI
# ═══════════════════════════════════════════════════════════════════════════

def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )

    # ── Input ────────────────────────────────────────────────────────────
    p.add_argument(
        "--input", required=True, type=Path, metavar="DIR",
        help="Run folder containing raw venue .jsonl / .jsonl.gz files.",
    )
    p.add_argument(
        "--symbol", default=None, metavar="SYM",
        help="Canonical symbol filter, e.g. BTCUSDT.  Default: all symbols.",
    )
    p.add_argument(
        "--venues", nargs="*", default=None, metavar="V",
        help="Restrict to a subset of venues, e.g. --venues binance bybit.",
    )
    p.add_argument(
        "--buffer-size", type=int, default=256, metavar="N",
        help="Book_state rows to prefetch per venue stream.  Default: 256.",
    )

    # ── Orderbook depth ──────────────────────────────────────────────────
    p.add_argument(
        "--book-depth", type=int, default=10, metavar="N",
        help="Number of price levels to retain per side.  Default: 10.",
    )
    p.add_argument(
        "--max-book-age-ms", type=float, default=10_000.0, metavar="MS",
        help="Evict a venue's book if no update in this many ms.  Default: 10000.",
    )
    p.add_argument(
        "--max-book-age-diff-ms", type=float, default=5_000.0, metavar="MS",
        help=(
            "Skip cross-venue comparison if the two books differ in age by more "
            "than this threshold.  Default: 5000."
        ),
    )

    # ── Detection filters ────────────────────────────────────────────────
    p.add_argument(
        "--min-spread-bps", type=float, default=0.0, metavar="BPS",
        help="Minimum spread in basis points to record a window.  Default: 0.",
    )
    p.add_argument(
        "--ttl-min-ms", type=float, default=0.0, metavar="MS",
        help="Only record arb windows lasting at least this many ms.  Default: 0.",
    )

    # ── Simulation ───────────────────────────────────────────────────────
    p.add_argument(
        "--latency-us", type=float, default=500.0, metavar="US",
        help="One-way network latency to exchange in microseconds.  Default: 500.",
    )
    p.add_argument(
        "--target-qty", type=float, default=0.0, metavar="QTY",
        help=(
            "Maximum fill quantity per leg in base units (e.g. 0.1 BTC).  "
            "0 means no cap – use all available qty at the best level.  Default: 0."
        ),
    )

    # ── Output ───────────────────────────────────────────────────────────
    p.add_argument(
        "--output", default=None, metavar="FILE",
        help="Optional CSV file to write all arb windows to.",
    )
    p.add_argument(
        "--show-arbs", action="store_true",
        help="Print each closed arb window to stdout as it is detected.",
    )
    p.add_argument(
        "--verbose", action="store_true",
        help="Print streaming progress to stderr every 500k events.",
    )
    p.add_argument(
        "--no-color", action="store_true",
        help="Disable ANSI colour codes in output.",
    )
    p.add_argument(
        "--max-windows", type=int, default=2_000_000, metavar="N",
        help=(
            "Maximum in-memory arb windows before flushing oldest half to CSV. "
            "Default: 2_000_000."
        ),
    )

    return p


def main() -> int:
    parser = build_arg_parser()
    args   = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
