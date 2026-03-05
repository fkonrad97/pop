# Persistence Schema

This document defines the JSONL schema written by `FilePersistSink`.

## File Format

- One JSON object per line (JSONL).
- Append-only.
- Records are emitted by `GenericFeedHandler` after successful parse/apply paths.
- Designed for downstream consumers that need both:
  - replay/debug from raw-normalized feed events (`snapshot`, `incremental`)
  - periodic state materialization (`book_state`)

## Common Fields

All record types include:

- `schema_version` (`int`): current value `1`.
- `event_type` (`string`): one of `snapshot`, `incremental`, `book_state`.
- `source` (`string`): origin path, e.g. `rest_snapshot`, `ws_snapshot`, `ws_incremental`, `snapshot_applied`, `incremental_applied`.
- `venue` (`string`): normalized venue name (`binance`, `okx`, `bitget`, `bybit`, `kucoin`).
- `symbol` (`string`): mapped runtime symbol from config.
- `persist_seq` (`uint64`): monotonically increasing sequence assigned by the sink per file stream.
- `ts_persist_ns` (`int64`): local wall-clock nanoseconds at write time.

### Ordering Semantics

For strict file order, use:

1. `persist_seq` (primary ordering key)
2. `ts_persist_ns` (diagnostics/tie-check)

For market-time-ish processing (local ingest order), use:

1. `ts_recv_ns` for `snapshot`/`incremental`
2. `ts_book_ns` for `book_state`

Note: `ts_recv_ns` and `ts_persist_ns` are local process clocks, not exchange timestamps.

## Time Fields

- `ts_recv_ns` (`int64`):
  - For `snapshot` / `incremental`: local wall-clock nanoseconds captured at message ingestion.
  - For `book_state`: set to `0` (not a direct ingress message).
- `ts_book_ns` (`int64`):
  - Present only for `book_state`.
  - Local wall-clock nanoseconds when the checkpoint event is created.

### Why Both `ts_book_ns` and `ts_persist_ns`?

- `ts_book_ns` marks when the orderbook checkpoint was generated in-memory.
- `ts_persist_ns` marks when the line was physically written.
- The gap helps diagnose persistence lag and backpressure.

## Record Types

### `snapshot`

Fields:

- `seq_first` (`uint64`) = snapshot update id.
- `seq_last` (`uint64`) = snapshot update id.
- `checksum` (`int64`).
- `bids` (`array<object>`), `asks` (`array<object>`).

Usage notes:
- Baseline event for state reconstruction.
- In most workflows, apply this first, then apply subsequent `incremental` events.

### `incremental`

Fields:

- `seq_first` (`uint64`).
- `seq_last` (`uint64`).
- `prev_last` (`uint64`).
- `checksum` (`int64`).
- `bids` (`array<object>`), `asks` (`array<object>`).

Usage notes:
- Delta update against the current in-memory book.
- `seq_first`/`seq_last` semantics depend on venue normalization details.
- `prev_last` is provided when available/derived to help continuity checks.

### `book_state`

Fields:

- `applied_seq` (`uint64`): controller applied sequence id at checkpoint time.
- `top_n` (`uint64`): depth persisted per side.
- `ts_book_ns` (`int64`): checkpoint event creation time.
- `bids` (`array<object>`), `asks` (`array<object>`).

Usage notes:
- Produced periodically (configured by `--persist_book_every_updates`).
- This is a derived checkpoint for fast restore/inspection.
- It is not a raw venue message, so `ts_recv_ns` is set to `0`.

## Level Object

Each `bids`/`asks` entry includes:

- `price` (`string`)
- `quantity` (`string`)
- `priceTick` (`int64`)
- `quantityLot` (`int64`)

## Example

```json
{"schema_version":1,"event_type":"incremental","source":"ws_incremental","venue":"binance","symbol":"btcusdt@depth@100ms","persist_seq":42,"ts_recv_ns":1740702000123456789,"ts_persist_ns":1740702000123467890,"seq_first":1234567890,"seq_last":1234567891,"prev_last":1234567889,"checksum":0,"bids":[{"price":"50000.10","quantity":"1.20","priceTick":5000010,"quantityLot":1200}],"asks":[{"price":"50001.20","quantity":"0.80","priceTick":5000120,"quantityLot":800}]}
```

Snapshot example:

```json
{"schema_version":1,"event_type":"snapshot","source":"ws_snapshot","venue":"okx","symbol":"BTC-USDT","persist_seq":7,"ts_recv_ns":1740702000000000000,"ts_persist_ns":1740702000000009000,"seq_first":987654321,"seq_last":987654321,"checksum":-123456789,"bids":[{"price":"50000.1","quantity":"1.0","priceTick":5000010,"quantityLot":1000}],"asks":[{"price":"50000.2","quantity":"0.9","priceTick":5000020,"quantityLot":900}]}
```

Book checkpoint example:

```json
{"schema_version":1,"event_type":"book_state","source":"incremental_applied","venue":"bitget","symbol":"BTCUSDT","persist_seq":120,"ts_recv_ns":0,"ts_book_ns":1740702000999000000,"ts_persist_ns":1740702000999012000,"applied_seq":4455667788,"top_n":10,"bids":[{"price":"50000.0","quantity":"2.0","priceTick":5000000,"quantityLot":2000}],"asks":[{"price":"50000.1","quantity":"1.5","priceTick":5000010,"quantityLot":1500}]}
```

## Consumer Quickstart

Minimal replay strategy:

1. Read lines in `persist_seq` order.
2. On `snapshot`, reset local book and apply full levels.
3. On `incremental`, apply deltas.
4. On `book_state`, optionally compare/replace local state for drift checks or fast restore.

If you only need latest recoverable state, tail to the newest `book_state` then replay later `incremental` events.
