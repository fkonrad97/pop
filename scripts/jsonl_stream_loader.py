#!/usr/bin/env python3
"""Stream `book_state` rows from persisted venue `.jsonl` / `.jsonl.gz` files.

This is intentionally small and reusable. It only solves raw input loading:

- discover venue files from a run folder
- open plain text or gzip-compressed files
- normalize venue/symbol names
- stream valid `book_state` rows sequentially
- optionally keep a small prefetch buffer per file

The loader does not build a unified orderbook and does not simulate anything.
"""

from __future__ import annotations

import argparse
import gzip
import json
from collections import deque
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Iterator


def normalize_symbol(symbol: str) -> str:
    """Normalize venue-specific symbol strings into one comparable identifier."""
    base = str(symbol).split("@", 1)[0]
    return "".join(ch for ch in base.upper() if ch.isalnum())


def normalize_venue_name(value: str) -> str:
    """Normalize venue strings so filenames, config values, and payload fields compare reliably."""
    return "".join(ch for ch in str(value).upper() if ch.isalnum()).lower()


def is_jsonl_path(path: Path) -> bool:
    name = path.name.lower()
    return name.endswith(".jsonl") or name.endswith(".jsonl.gz")


def venue_from_path(path: Path) -> str:
    name = path.name
    if name.lower().endswith(".jsonl.gz"):
        name = name[:-9]
    elif name.lower().endswith(".jsonl"):
        name = name[:-6]
    return normalize_venue_name(name)


def open_text(path: Path):
    """Open raw persisted venue files as text, including gzip-compressed files."""
    if path.name.lower().endswith(".gz"):
        return gzip.open(path, "rt", encoding="utf-8")
    return path.open("rt", encoding="utf-8")


def discover_inputs(input_root: Path, venues: list[str] | None = None) -> list[Path]:
    """Discover raw venue files from one run folder."""
    if not input_root.exists() or not input_root.is_dir():
        return []
    allowed = {normalize_venue_name(item) for item in venues} if venues else None
    inputs: list[Path] = []
    for path in sorted(input_root.iterdir()):
        if not path.is_file() or not is_jsonl_path(path):
            continue
        venue = venue_from_path(path)
        if allowed is not None and venue not in allowed:
            continue
        inputs.append(path)
    return inputs


@dataclass
class RawBookStateStream:
    """Sequential reader for one raw venue file.

    The reader skips non-`book_state` events and yields only rows that are
    structurally useful for downstream orderbook/simulation work.
    """

    path: Path
    symbol_filter: str | None = None
    buffer_size: int = 1
    line_no: int = 0
    venue: str = field(init=False)
    handle: Any | None = field(default=None, init=False)
    buffer: deque[dict[str, Any]] = field(default_factory=deque, init=False)

    def __post_init__(self) -> None:
        self.venue = venue_from_path(self.path)
        if self.symbol_filter:
            self.symbol_filter = normalize_symbol(self.symbol_filter)
        self.buffer_size = max(1, int(self.buffer_size))

    def open(self) -> None:
        if self.handle is not None:
            return
        self.handle = open_text(self.path)
        skipped = 0
        while skipped < self.line_no:
            line = self.handle.readline()
            if not line:
                break
            skipped += 1
        self.line_no = skipped

    def close(self) -> None:
        if self.handle is not None:
            self.handle.close()
            self.handle = None

    def _fill_buffer(self) -> None:
        self.open()
        while len(self.buffer) < self.buffer_size:
            line = self.handle.readline()
            if not line:
                break
            self.line_no += 1
            row = self._parse_line(line)
            if row is not None:
                self.buffer.append(row)

    def _parse_line(self, line: str) -> dict[str, Any] | None:
        line = line.strip()
        if not line:
            return None
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            return None
        if obj.get("event_type") != "book_state":
            return None

        payload_venue = normalize_venue_name(obj.get("venue", self.venue))
        symbol = str(obj.get("symbol", ""))
        canonical_symbol = normalize_symbol(symbol)
        if self.symbol_filter and canonical_symbol != self.symbol_filter:
            return None

        bids = obj.get("bids", [])
        asks = obj.get("asks", [])
        if not bids or not asks:
            return None

        try:
            ts_book_ns = int(obj.get("ts_book_ns", 0))
            top_n = int(obj.get("top_n", 0))
            best_bid_price = float(bids[0]["price"])
            best_bid_qty = float(bids[0]["quantity"])
            best_ask_price = float(asks[0]["price"])
            best_ask_qty = float(asks[0]["quantity"])
        except (KeyError, TypeError, ValueError):
            return None
        if ts_book_ns <= 0:
            return None

        return {
            "venue": payload_venue,
            "symbol": symbol,
            "canonical_symbol": canonical_symbol,
            "ts_book_ns": ts_book_ns,
            "top_n": top_n,
            "source_path": str(self.path),
            "best_bid_price": best_bid_price,
            "best_bid_qty": best_bid_qty,
            "best_ask_price": best_ask_price,
            "best_ask_qty": best_ask_qty,
            "bids": bids,
            "asks": asks,
        }

    def next_row(self) -> dict[str, Any] | None:
        """Return the next valid `book_state` row for this file."""
        if not self.buffer:
            self._fill_buffer()
        if not self.buffer:
            return None
        row = self.buffer.popleft()
        if not self.buffer:
            self._fill_buffer()
        return row

    def __iter__(self) -> Iterator[dict[str, Any]]:
        while True:
            row = self.next_row()
            if row is None:
                break
            yield row


def iter_run_book_states(
    input_root: Path,
    symbol_filter: str | None = None,
    venues: list[str] | None = None,
    buffer_size: int = 1,
) -> Iterator[tuple[str, dict[str, Any]]]:
    """Yield `(venue, row)` pairs for all selected venue files.

    Files are streamed independently and sequentially in filename order. This is
    a loader helper, not a global timestamp merge.
    """

    for path in discover_inputs(input_root, venues):
        stream = RawBookStateStream(path=path, symbol_filter=symbol_filter, buffer_size=buffer_size)
        try:
            for row in stream:
                yield stream.venue, row
        finally:
            stream.close()


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--input",
        dest="input_root",
        required=True,
        type=Path,
        help="Run folder containing raw venue .jsonl / .jsonl.gz files.",
    )
    parser.add_argument(
        "--symbol",
        dest="symbol_filter",
        default=None,
        help="Optional canonical symbol filter, for example BTCUSDT.",
    )
    parser.add_argument(
        "--venues",
        nargs="*",
        default=None,
        help="Optional venue subset, for example --venues binance okx bybit.",
    )
    parser.add_argument(
        "--buffer-size",
        type=int,
        default=64,
        help="Number of valid book_state rows to prefetch per file.",
    )
    parser.add_argument(
        "--limit",
        type=int,
        default=20,
        help="Maximum number of rows to print in demo mode.",
    )
    return parser


def main() -> int:
    parser = build_arg_parser()
    args = parser.parse_args()

    input_paths = discover_inputs(args.input_root, args.venues)
    print(
        {
            "input_root": str(args.input_root),
            "files_found": len(input_paths),
            "files": [str(path) for path in input_paths],
            "symbol_filter": normalize_symbol(args.symbol_filter) if args.symbol_filter else None,
            "venues": [normalize_venue_name(v) for v in args.venues] if args.venues else [],
            "buffer_size": int(args.buffer_size),
        }
    )

    shown = 0
    for venue, row in iter_run_book_states(
        input_root=args.input_root,
        symbol_filter=args.symbol_filter,
        venues=args.venues,
        buffer_size=args.buffer_size,
    ):
        print(
            {
                "venue": venue,
                "symbol": row["canonical_symbol"],
                "ts_book_ns": row["ts_book_ns"],
                "best_bid_price": row["best_bid_price"],
                "best_ask_price": row["best_ask_price"],
                "source_path": row["source_path"],
            }
        )
        shown += 1
        if shown >= int(args.limit):
            break
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
