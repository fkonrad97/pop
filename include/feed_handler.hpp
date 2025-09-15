#pragma once            // https://en.wikipedia.org/wiki/Pragma_once
#include <cstdint>
#include <string>
#include <vector>
#include <string_view>  // for resolve_symbol(...)
#include <utility>      // for std::pair in vectors
#include <chrono>

namespace md {
    /**
     * @typedef VenueId
     * @brief Stable numeric identifier for an exchange/venue (e.g., 1=BINANCE, 2=KRAKEN).
     *
     * Notes:
     *  - Assigned by the system (not by the venue). Keep stable across runs.
     *  - 16-bit is ample for dozens/hundreds of venues.
     */
    using VenueId = std::uint16_t;

    /**
     * @typedef InstrumentId
     * @brief Internal numeric identifier for a tradable instrument/symbol (e.g., BTCUSDT → 1001).
     *
     * Notes:
     *  - Map venue symbols → InstrumentId via a symbol map.
     *  - Prefer not to reuse IDs while any state for that instrument is live.
     */
    using InstrumentId = std::uint32_t;

    /**
     * @enum Status
     * @brief Immediate return code from API calls (accept/reject of a request).
     *
     * Semantics:
     *  - Methods are typically non-blocking; `OK` means the request was accepted
     *    and will be executed by the handler’s I/O thread. Results arrive via sinks.
     */
    enum class Status : std::uint8_t {
        OK,              ///< Accepted.
        INVALID_STATE,   ///< Call not allowed in current state (e.g., start() while running).
        UNKNOWN_SYMBOL,  ///< InstrumentId not known/not subscribed.
        NOT_RUNNING,     ///< Handler not started or already stopped.
        BUSY,            ///< Temporarily unable to accept (e.g., rate-limited/backpressure).
        UNSUPPORTED,     ///< Feature not supported by this venue/implementation.
        ERROR            ///< Generic failure (see logs/metrics for details).
    };

    /**
     * @enum State
     * @brief High-level lifecycle state of the feed handler.
     *
     * Typical flow:
     *   DISCONNECTED → CONNECTING → SUBSCRIBED → SNAPSHOTTING → REPLAYING → STREAMING
     *   On gaps/staleness: STREAMING → RESYNCING → SNAPSHOTTING → REPLAYING → STREAMING
     */
    enum class State : std::uint8_t {
        DISCONNECTED,  ///< No active connections.
        CONNECTING,    ///< Establishing TCP/TLS/WS.
        SUBSCRIBED,    ///< Subscriptions sent; incrementals buffering.
        SNAPSHOTTING,  ///< Fetching initial/resync snapshot (baseline).
        REPLAYING,     ///< Replaying buffered deltas > snapshot seq.
        STREAMING,     ///< Steady-state: delivering in-order deltas.
        RESYNCING      ///< Recovering from gap/stale; will snapshot again.
    };

    /**
     * @enum ChannelBits
     * @brief Bitmask flags selecting which market-data streams to subscribe to.
     *
     * Combine flags with bitwise OR (|); test with bitwise AND (&).
     *
     * Flags:
     *  - DEPTH_L1 : Top-of-Book (best bid/ask). Lowest bandwidth; no depth.
     *  - DEPTH_L2 : Aggregated depth by price level (L2 / Top-K). Used for books.
     *  - TRADES   : Trade prints (price/size). Optional, for analytics/cold path.
     *
     * Notes:
     *  - Some venues have distinct endpoints for L1 vs L2; others derive L1 from L2.
     *    Keeping them separate lets you choose bandwidth/CPU trade-offs.
     *  - Passing 0 (no flags) typically results in unsubscribing from all streams.
     *
     * Example:
    * @code
    * ChannelsMask mask = DEPTH_L2 | TRADES;
    * if (mask & DEPTH_L2) {
    *     /* subscribe depth *\/
    * }
    * @endcode
    */
    enum ChannelBits : std::uint32_t {
        DEPTH_L1 = 1u << 0,  ///< Top-of-Book (best bid/ask)
        DEPTH_L2 = 1u << 1,  ///< Aggregated depth by price level (Top-K)
        TRADES   = 1u << 2,  ///< Trade prints
    };
    /**
    * @typedef ChannelsMask
    * @brief Bitmask type holding a combination of @ref ChannelBits.
    */
    using ChannelsMask = std::uint32_t;

    /**
    * @struct Timeouts
    * @brief Thresholds for connection, subscription, and liveness checks.
    *
    * These are configuration values only; the handler’s state machine decides how
    * to act when a threshold is exceeded (e.g., mark STALE, resubscribe, reconnect).
    *
    * Fields:
    *  - connect_ms
    *      Upper bound to complete TCP + TLS + WebSocket upgrade.
    *      If exceeded, the implementation typically retries with backoff/failover.
    *
    *  - subscribe_ms
    *      Time to wait after sending subscriptions for an ACK or first data frame.
    *      If exceeded, the implementation may retry the subscription.
    *
    *  - heartbeat_ms
    *      Expected interval between venue heartbeats/keepalives.
    *      Missing a heartbeat around this window increments a miss counter.
    *
    *  - read_idle_ms
    *      Max time with **no inbound data at all** (payload or heartbeat).
    *      Exceeding this usually transitions health to STALE and may trigger resync/reconnect.
    */
    struct Timeouts {
        int connect_ms   {3000};
        int subscribe_ms {3000};
        int heartbeat_ms {10000};
        int read_idle_ms {15000};
    };

    /**
    * @struct Backoff
    * @brief Configuration for retry delays (used by reconnect/snapshot retry logic).
    *
    * This struct holds the tunables; the exact retry schedule is defined by the
    * handler implementation (e.g., exponential with jitter).
    *
    * Fields:
    *  - base_ms : initial delay before the first retry attempt.
    *  - cap_ms  : maximum delay cap between consecutive retries.
    *  - jitter  : randomization fraction applied to the computed delay (0.0..1.0).
    */
    struct Backoff {
        int   base_ms {200};   ///< First retry delay (milliseconds).
        int   cap_ms  {2000};  ///< Maximum delay cap (milliseconds).
        double jitter {0.20};  ///< Randomization fraction (e.g., 0.20 = ±20%).
    };

    /**
     * @enum SnapshotSource
     * @brief Selects how the initial (and resync) order-book snapshot is obtained.
     *
     * A snapshot is a one-off, point-in-time **baseline** of the order book used to start or restore a correct book lineage.
     *
     * @par When to use REST:
     * - Venue exposes a REST depth endpoint
     * - Simple and reliable; independent of WS stream health
     * - Latency (single HTTP round-trip) is hidden by the buffer→replay step
     * - Watch for rate limits; implement backoff/jitter
     *
     * @par When to use WS_SNAPSHOT:
     * - Venue provides a snapshot **on WebSocket** (single frame or dedicated channel)
     * - Avoids an extra HTTP call; can reduce cold-start time
     * - Semantics vary by venue; ensure snapshot carries (or can be aligned to) a seq
     * - If the chosen source fails (timeout/429/etc.), implementations MAY **fallback** to the other source per policy
     *
     * @par Runtime behavior & guarantees:
     * - Changing this setting at runtime SHOULD trigger a **resync**
     *
     * @warning Never deliver incrementals before a valid snapshot is applied for the current lineage; doing so risks a corrupt in-memory book downstream
     */
    enum class SnapshotSource : std::uint8_t {
        REST,           ///< Fetch snapshot via REST/HTTP depth endpoint.
        WS_SNAPSHOT     ///< Fetch snapshot via a WebSocket snapshot frame/channel (if offered)
    };

    /**
     * @struct Filters
     * @brief Per-instrument exchange constraints used to normalize price/quantity to discrete integer grids (ticks/lots) and to validate orders/data.
     *
     * Many venues restrict prices and quantities to fixed increments and minima.
     *
     * @par Fields
     * - px_tick: Smallest valid **price** increment in quote currency units, (e.g., 0.01 USDT for BTC/USDT)
     * - qty_step: Smallest valid **quantity** increment in base currency units, (e.g., 0.0001 BTC)
     * - min_qty: Minimum allowed order **quantity** in base units (>= qty_step). If the venue has no rule or it’s unknown, 0.0
     * - min_notional: Minimum allowed **notional** per order in quote units (price × qty). If the venue has no rule or it’s unknown, 0.0
     *
     * @par Normalization
     * Convert venue floats/decimals to integers using these grids:
     *      ticks = llround(price / px_tick)
     *      lots  = (long long) floor(qty / qty_step)
     *
     * Convert back to native units when needed:
     *      price = ticks * px_tick
     *      qty   = lots  * qty_step
     *
     * @par Dynamic filters
     * Some venues use **price bands** (tick size changes with price). If filters
     * change at runtime, bump the instrument’s snapshot lineage
     * and re-handshake (snapshot → replay) to avoid mixed rules within one book.
     *
     * @par Example (BTC/USDT)
     *   // Given venue filters:
     *   px_tick = 0.01
     *   qty_step = 0.0001
     *   min_qty = 0.0001
     *   min_notional = 5.00
     *
     *   // A. Normalizing incoming market data from the venue
     *   // (venue quotes should already be on the grid; allow tiny FP noise)
     *   venue_price = 60100.03
     *   ticks = llround(60100.03 / 0.01) = 6,010,003
     *   normalized_price = ticks * 0.01 = 60100.03
     *
     *   venue_qty = 0.012345
     *   lots = (long long) floor(0.012345 / 0.0001) = floor(123.45) = 123
     *   normalized_qty = 123 * 0.0001 = 0.0123
     *
     *   // B. Placing a limit order
     *   desired_limit_price = 60100.034   // off-grid user input
     *   bid_limit_ticks = floor(60100.034 / 0.01) = floor(6,010,003.4) = 6,010,003
     *   bid_limit_price = 6,010,003 * 0.01 = 60100.03   // BID snaps down
     *
     *   ask_limit_ticks = ceil(60100.034 / 0.01) = ceil(6,010,003.4) = 6,010,004
     *   ask_limit_price = 6,010,004 * 0.01 = 60100.04   // ASK snaps up
     *
     *   order_qty_requested = 0.000087
     *   lots = floor(0.000087 / 0.0001) = floor(0.87) = 0
     *   // 0 lots is invalid; enforce min step:
     *   lots = 1  -> normalized_qty = 1 * 0.0001 = 0.0001
     *
     *   // C. Validate min_qty and min_notional
     *   normalized_qty (0.0001) >= min_qty (0.0001)  -> OK
     *   notional = bid_limit_price * normalized_qty
     *            = 60100.03 * 0.0001 = 6.010003
     *   notional (6.010003) >= min_notional (5.00)   -> OK
     */
    struct Filters {
        double px_tick{0.0};        ///< Price tick size in quote units (e.g., 0.01 USDT). > 0 required.
        double qty_step{0.0};       ///< Quantity step size in base units (e.g., 0.0001 BTC). > 0 required.
        double min_qty{0.0};        ///< Minimum order quantity in base units (0.0 if none/unknown).
        double min_notional{0.0};   ///< Minimum notional (price*qty) in quote units (0.0 if none/unknown)
    };

    /**
    * @enum Side
    * @brief Order-book side for a price-level delta.
    *
    * Semantics:
    *  - BID: buy side. Price levels are sorted high→low; best = highest bid.
    *  - ASK: sell side. Price levels are sorted low→high; best = lowest ask.
    *
    * Notes:
    *  - Rounding when converting floats to integer ticks depends on side:
    *      * BID  → floor(price / px_tick)
    *      * ASK  → ceil (price / px_tick)
    *  - Side determines where a delta is applied and how L1 (best bid/ask) evolves.
    */
    enum class Side : std::uint8_t { BID, ASK };

    /**
    * @enum Op
    * @brief Operation applied to a single L2 (price-level) entry.
    *
    *  - UPSERT: Set the *aggregate* visible size at the given price level. If the
    *            level does not exist, create it; if it exists, overwrite its size.
    *            If sz_lots == 0, treat as DELETE (remove the level).
    *  - DELETE: Remove the price level entirely (size is ignored).
    *
    * Why:
    *  - Venues differ: some send absolute sizes per level; others send deltas
    *    (±quantity) or per-order L3 changes. The venue adapter MUST convert those
    *    wire formats into these canonical L2 operations so downstream book logic
    *    is consistent and fast.
    *
    * Invariants:
    *  - After applying a delta, levels on BID remain strictly descending by price,
    *    levels on ASK remain strictly ascending. If a sequence gap would violate
    *    this (e.g., bid > ask), trigger resync rather than applying out-of-order.
    *
    * Examples (prices shown in native units for readability; actual API uses integer ticks/lots):
    *  Pre:
    *    Bids: 100.00×10,  99.99×8
    *    Asks: 100.01×6,   100.02×12
    *
    *  1) { side=BID, op=UPSERT, px=100.00, size=7 }
    *     → Bids: 100.00×7, 99.99×8           (overwrite aggregate size at 100.00)
    *
    *  2) { side=ASK, op=UPSERT, px=100.01, size=0 }
    *     → Asks: 100.02×12                   (size=0 treated as DELETE)
    *
    *  3) { side=BID, op=UPSERT, px=100.02, size=3 }
    *     → Bids: 100.02×3, 100.00×7, 99.99×8 (new level inserted; becomes best bid)
    *
    *  4) { side=ASK, op=DELETE, px=100.02 }
    *     → Asks: (empty)                     (remove level entirely)
    */
    enum class Op   : std::uint8_t { UPSERT, DELETE }; // UPSERT = set aggregate size at price

    /**
    * @struct PriceLevel
    * @brief Normalized representation of a single L2 (price-level) entry.
    *
    * Prices and quantities are stored as **integers** on the venue’s discrete grids:
    *  - px_ticks = price / px_tick
    *  - sz_lots  = quantity / qty_step
    *
    * This avoids floating-point drift and makes book operations fast and exact.
    * The conversion grid comes from @ref Filters (px_tick, qty_step).
    *
    * @par Fields
    * - px_ticks
    *   Price expressed in integer ticks (e.g., 60100.03 @ px_tick=0.01 → 6,010,003).
    *   Creation/normalization rule:
    *     * For data coming from the venue (already on-grid): px_ticks = llround(price / px_tick).
    *     * For order placement or snapping: BID uses floor(price/px_tick), ASK uses ceil(price/px_tick).
    *
    * - sz_lots
    *   Aggregate visible size at this price, in integer lots (e.g., 0.0123 @ qty_step=0.0001 → 123).
    *   Normalization rule: sz_lots = floor(qty / qty_step).
    *
    * @par Usage
    * - In @ref OrderBookSnapshot, arrays of PriceLevel form the initial L2 state
    *   (Top-K per side). Snapshot entries MUST have sz_lots > 0.
    * - In @ref OrderBookDelta, the fields are carried directly (with side/op in the delta).
    *   For deltas, sz_lots==0 is treated as DELETE; see @ref Op.
    *
    * @par Conversions
    *   price_native  = px_ticks * px_tick
    *   qty_native    = sz_lots  * qty_step
    *
    * @par Examples
    *   Filters: { px_tick = 0.01, qty_step = 0.0001 }
    *
    *   Venue quote  : price=60100.03, qty=0.012345
    *     px_ticks = llround(60100.03 / 0.01)  = 6'010'003
    *     sz_lots  = floor(0.012345 / 0.0001) = 123  -> qty_native = 0.0123
    *
    *   User limit (snap to grid):
    *     BID limit 60100.034 → floor(60100.034/0.01)=6'010'003 → price=60100.03
    *     ASK limit 60100.034 →  ceil(60100.034/0.01)=6'010'004 → price=60100.04
    */
    struct PriceLevel {
        std::int64_t px_ticks{0};   // price / px_tick
        std::int64_t sz_lots{0};    // qty / qty_step
    };

    /**
    * @struct EventMeta
    * @brief Common metadata attached to every market-data event (snapshot, delta, trade).
    *
    * Carries identity, lineage, ordering, and timing information so downstream
    * components (Book Writer, Health Monitor) can verify integrity, detect gaps,
    * and measure freshness/latency.
    *
    * @par Fields
    * - venue
    *   Static exchange identifier (e.g., BINANCE=1). Useful for routing/metrics.
    *
    * - inst
    *   Internal instrument identifier (your symbol map), not the venue’s raw symbol.
    *
    * - seq
    *   Venue-provided **sequence number** for ordering **within the current lineage**
    *   (see @ref snapshot_ver). The Feed Handler guarantees that, for a given
    *   `(venue, inst, snapshot_ver)`, delivered deltas are strictly increasing in
    *   `seq` with no duplicates. If the venue lacks a usable sequence, the handler
    *   MUST synthesize one that preserves order through the snapshot→replay→stream
    *   handshake.
    *
    * - snapshot_ver
    *   Monotonic **lineage counter** incremented every time the book is rebuilt
    *   from a fresh snapshot (startup or resync), or when normalization rules
    *   change (e.g., filters/tick sizes updated). All events that belong to the
    *   same coherent book instance share the same `snapshot_ver`.
    *
    * - ts_exch_ns
    *   Exchange timestamp in **nanoseconds** since epoch **if** provided by the
    *   venue; otherwise `0`. Precision/meaning is venue-specific (ms/us/ns). The
    *   adapter should convert to ns. Not guaranteed monotonic. Estimate wire latency:
    *     `approx_latency = ts_recv_ns - ts_exch_ns`  (only if clocks are sane).
    *
    * - ts_recv_ns
    *   Local **monotonic** receive time (nanoseconds) captured immediately upon decode.
    *   The epoch is implementation-defined; compare only within the same process.
    *
    * @par Invariants (enforced by the Feed Handler)
    * - For a fixed `(venue, inst, snapshot_ver)`:
    *     * The lineage begins with exactly one `OrderBookSnapshot`.
    *     * Subsequent `OrderBookDelta` events are delivered in strictly increasing
    *       `seq` order (no gaps). On gap/out-of-order beyond a small buffer,
    *       the handler triggers resync (new `snapshot_ver`).
    * - `ts_recv_ns` of emitted events is non-decreasing in delivery order.
    * - `ts_exch_ns` may be absent or non-monotonic; never enforce ordering by it.
    */
    struct EventMeta {
        VenueId      venue{0};
        InstrumentId inst{0};
        std::uint64_t seq{0};           ///< Venue sequence within current lineage
        std::uint64_t snapshot_ver{0};  ///< ++ on each fresh snapshot/rebuild (lineage id)
        std::int64_t  ts_exch_ns{0};    ///< Exchange timestamp in ns since epoch (0 if absent)
        std::int64_t  ts_recv_ns{0};    ///< Local monotonic receive time in ns
    };

    /**
    * @struct OrderBookSnapshot
    * @brief One-time **baseline** of the L2 order book (Top-K per side) that
    *        starts a new lineage (`m.snapshot_ver`) for an instrument.
    *
    * The Feed Handler emits exactly one snapshot at the beginning of a lineage
    * (startup or resync). All subsequent @ref OrderBookDelta events with the same
    * `snapshot_ver` apply on top of this baseline in strictly increasing `m.seq`.
    *
    * @par Context
    * - Represents the **aggregated price-level** book (L2), normalized to
    *   integer grids: price in @ref PriceLevel::px_ticks, quantity in
    *   @ref PriceLevel::sz_lots (see @ref Filters).
    * - Arrays are **already sorted**:
    *     * `bids_desc`: strictly descending by price (best bid at index 0).
    *     * `asks_asc` : strictly ascending by price (best ask at index 0).
    * - Levels are **unique** by price per side; quantities are **strictly > 0**
    *   in a snapshot (no zero-size levels).
    * - `depth_k` indicates “levels per side included”. The vectors may be
    *   shorter than `depth_k` if the venue has fewer levels at that moment.
    *
    * @par Invariants
    * - `bids_desc` strictly desc by `px_ticks`; `asks_asc` strictly asc.
    * - No duplicate `px_ticks` within a side; all `sz_lots > 0`.
    * - If both sides non-empty: `best_bid_px_ticks < best_ask_px_ticks`
    *   (no crossed book); otherwise handler should resync.
    * - `m.seq` equals the venue’s **snapshot sequence** (or a synthetic `S`)
    *   that aligns with subsequent deltas (replay only deltas with `seq > S`).
    *
    * @par Example (Top-3 per side; integers shown as native for readability)
    *   depth_k = 3
    *   bids_desc = { (100.02, 3.1), (100.01, 5.0), (100.00, 2.0) }
    *   asks_asc  = { (100.03, 1.2), (100.04, 4.8), (100.05, 7.0) }
    */
    struct OrderBookSnapshot {
        EventMeta m;
        std::uint16_t depth_k{0};                 // levels per side included
        std::vector<PriceLevel> bids_desc;        // highest -> lowest
        std::vector<PriceLevel> asks_asc;         // lowest  -> highest
        Filters filters{};                        // active filters for this instrument
    };

    /**
    * @struct OrderBookDelta
    * @brief A single **incremental change** to the L2 (price-level) order book.
    *
    * Deltas apply on top of the most recent @ref OrderBookSnapshot within the same
    * lineage (`m.snapshot_ver`). They are delivered by the Feed Handler in strictly
    * increasing `m.seq` order (no gaps/dupes) for a given `(venue, inst, snapshot_ver)`.
    *
    * @par Context
    * - @ref side : Which side of the book the change targets (BID or ASK).
    * - @ref op   : Operation on the price level
    * - @ref px_ticks : Price expressed as integer ticks (price / px_tick).
    * - @ref sz_lots  : Aggregate size at that price, in integer lots (qty / qty_step).
    *
    * @par Canonicalization
    * Venues may publish absolute L2 sizes, +/- size deltas, or per-order L3 events.
    * The venue adapter MUST convert those wire formats into this canonical L2 model:
    *     - absolute → UPSERT(px, size)
    *     - delta    → compute new aggregate → UPSERT(px, new_size) or DELETE if 0
    *     - L3       → maintain per-price sum → UPSERT/DELETE accordingly
    *
    * @par Examples (prices shown in native units for readability; actual code uses ticks/lots)
    *   Pre:
    *     Bids: 100.00×10,  99.99×8
    *     Asks: 100.01×7,   100.02×12
    *
    *   1) { side=BID, op=UPSERT, px=100.00, size=7 }
    *      → Bids: 100.00×7, 99.99×8         (overwrite aggregate at 100.00)
    *
    *   2) { side=ASK, op=UPSERT, px=100.01, size=0 }
    *      → Asks: 100.02×12                 (size=0 → treat as DELETE)
    *
    *   3) { side=BID, op=UPSERT, px=100.02, size=3 }
    *      → Bids: 100.02×3, 100.00×7, 99.99×8   (insert new best bid)
    *
    *   4) { side=ASK, op=DELETE, px=100.02 }
    *      → Asks: (empty)                    (remove level entirely)
    */
    struct OrderBookDelta {
        EventMeta m;
        Side side{Side::BID};
        Op   op{Op::UPSERT};
        std::int64_t px_ticks{0};
        std::int64_t sz_lots{0};    ///< Ignored when op=DELETE; 0 with UPSERT implies DELETE
    };

    /**
    * @struct TradeTick
    * @brief Normalized **trade print** (optional market-data event).
    *
    * Purpose:
    *  - Cold-path analytics: last price/volume, bars/VWAP, sanity checks.
    *  - Does **not** mutate the book (depth is maintained via Snapshot/Delta).
    *
    * Fields:
    *  - m        : common metadata (venue, inst, seq, snapshot_ver, timestamps).
    *  - px_ticks : trade price in integer ticks (price / px_tick). See Filters.
    *  - qty_lots : trade size  in integer lots  (qty   / qty_step). See Filters.
    *
    * Notes:
    *  - Some venues sequence trades separately from depth; the handler may
    *    synthesize `m.seq` to preserve delivery order. Do not use trades to
    *    drive order-book updates.
    *  - If aggressor (BUY/SELL) is needed later, it can be added as an
    *    optional field without changing current consumers.
    */
    struct TradeTick {
        EventMeta m;
        std::int64_t px_ticks{0};
        std::int64_t qty_lots{0};
    };

    /**
    * @enum FeedHealth
    * @brief Health state of a venue market-data stream.
    *
    * Intended use:
    *  - Drive routing/gating upstream (e.g., Central Brain should avoid signals from
    *    non-HEALTHY venues; may allow DEGRADED with wider thresholds).
    *  - Emit via @ref FeedStatus on state changes and periodic heartbeats.
    *
    * States:
    *  - HEALTHY   : Data flowing, seq contiguous, staleness within target.
    *  - DEGRADED  : Data flowing but quality marginal (e.g., rising latency/jitter,
    *                shallow reorder buffering, minor drops). Watch closely.
    *  - STALE     : No payload/heartbeat within read_idle threshold; book freshness
    *                suspect. Prepare to resync or reconnect.
    *  - RESYNCING : Snapshot→replay in progress after a seq GAP or staleness event.
    *                Do not consume this venue for trading during this state.
    *  - DOWN      : Disconnected or hard error; retries/backoff underway.
    */
    enum class FeedHealth : std::uint8_t {
        HEALTHY,
        DEGRADED,
        STALE,
        RESYNCING,
        DOWN
    };

    /**
    * @struct FeedStatus
    * @brief Health update for a venue’s market-data handler.
    *
    * Emitted on **state changes** (@ref FeedHealth) and optionally on a
    * periodic heartbeat (e.g., 1s) so upstream can gate signals and monitor liveness.
    *
    * Fields:
    *  - venue   : exchange identifier.
    *  - health  : current state (HEALTHY/DEGRADED/STALE/RESYNCING/DOWN).
    *  - reason  : short cause/annotation, e.g. "GAP", "STALE", "RECONNECT",
    *              "SNAPSHOT_TIMEOUT", "RATE_LIMIT", etc. May be empty on heartbeats.
    *
    * Notes:
    *  - Keep messages lightweight; richer counters/metrics can live in a separate stats struct.
    *  - If health worsens (e.g., to STALE/RESYNCING/DOWN), consumers should avoid
    *    trading on this venue until back to HEALTHY (or DEGRADED, per policy).
    */
    struct FeedStatus {
        VenueId      venue{0};
        FeedHealth   health{FeedHealth::DOWN};
        std::string  reason;     ///< e.g., "GAP", "STALE", "RECONNECT"; empty for routine heartbeat
    };

    /**
     * @struct FeedHandlerStaticConfig
     * @brief **Static** settings for a venue feed handler.
     *
     * Fields:
     *  - venue        : Stable numeric venue id (e.g., 1=BINANCE).
     *  - venue_name   : Human-readable label (for logs/metrics).
     *  - ws_urls      : WebSocket endpoints (primary + failovers) for MD.
     *  - rest_snapshot_url : HTTP endpoint for snapshots (if SnapshotSource::REST).
     *  - snapshot_source   : Where to fetch snapshots (REST or WS_SNAPSHOT).
     *  - tls          : Enable TLS on network connections.
     *  - cert_pins_spki : Optional SPKI pins for certificate pinning.
     */
    struct FeedHandlerStaticConfig {
        VenueId venue{0};
        std::string venue_name;
        std::vector<std::string> ws_urls;
        std::string rest_snapshot_url;
        SnapshotSource snapshot_source{SnapshotSource::REST};
        bool tls{true};
        std::vector<std::string> cert_pins_spki;
    };

    /**
     * @struct FeedHandlerHotConfig
     * @brief **Hot-reloadable** settings (safe to change at runtime).
     *
     * Applying a diff of these fields should not require a process restart. Some
     * changes (e.g., filters/depth) SHOULD trigger a per-instrument resync
     * (bump `snapshot_ver`, do snapshot→replay).
     *
     * Fields:
     *  - channels   : Bitmask of subscribed streams (DEPTH_L2, TRADES, ...).
     *  - depth_k    : Target Top-K per side to maintain/emit.
     *  - instruments: Watch list by InstrumentId (alternative to `symbols`).
     *  - symbols    : Watch list by venue symbol (adapter maps to ids).
     *  - filters_by_id / filters_by_symbol :
     *                Per-instrument normalization filters (px_tick, qty_step, ...).
     *                **Changing filters requires resync** of affected instruments.
     *  - timeouts   : Connect/subscribe/heartbeat/read-idle thresholds.
     *  - reconnect_backoff / snapshot_backoff :
     *                Retry tunables for WS connect and snapshot fetch.
     *  - conflate   : Enable last-value conflation toward the Book Writer.
     *  - conflation_window_ms :
     *                Max hold time for conflation before flushing.
     *  - reorder_buffer :
     *                Capacity for out-of-order smoothing (by seq).
     */
    struct FeedHandlerHotConfig {
        ChannelsMask channels{DEPTH_L2};
        std::uint16_t depth_k{25};

        std::vector<InstrumentId> instruments;
        std::vector<std::string>  symbols;

        std::vector<std::pair<InstrumentId, Filters>> filters_by_id;
        std::vector<std::pair<std::string,   Filters>> filters_by_symbol;

        Timeouts timeouts{};
        Backoff  reconnect_backoff{};
        Backoff  snapshot_backoff{};

        bool        conflate{true};
        int         conflation_window_ms{10};
        std::size_t reorder_buffer{64};
    };

    /**
     * @struct FeedHandlerConfig
     * @brief Top-level config container (static + hot sections).
     *
     * Usage:
     *  - Load once at startup (init with `s` + `h`).
     *  - On file change, compute diff on `h` and apply via IFeedHandler setters:
     *      set_channels/depth_k/timeouts/backoff/conflation/reorder_buffer,
     *      add/remove_instruments, set_filters(_bulk).
     *  - Treat changes in `s` as **static**: plan reconnect or restart.
     *
     * Fields:
     *  - schema_version : Config file schema; bump on incompatible changes.
     *  - s              : Static config (rarely changed).
     *  - h              : Hot-reloadable config (runtime tweaks).
     */
    struct FeedHandlerConfig {
        int schema_version{1};
        FeedHandlerStaticConfig s;
        FeedHandlerHotConfig    h;
    };

    /**
    * @struct IBookWriterSink
    * @brief Target for normalized book events emitted by a FeedHandler.
    *
    * Threading & performance:
    *  - Callbacks are invoked from the FeedHandler’s I/O thread. **Do not block**
    *  - Implementations should enqueue work to an internal single-writer data
    *    structure (e.g., lock-free ring/MPSC queue) and return immediately.
    *  - Callbacks must be **exception-safe**; do not throw across this boundary.
    *
    * Delivery semantics (per {venue, inst, snapshot_ver}):
    *  - Exactly one @ref on_snapshot at the start of a lineage.
    *  - Followed by many @ref on_delta in strictly increasing m.seq (gap-free).
    *  - Cross-instrument ordering is not guaranteed.
    *  - @ref on_trade is optional and independent of depth; trades do not mutate the book.
    *
    * Lifetime:
    *  - The sink pointer is installed via IFeedHandler::set_book_sink(...) and must
    *    remain valid until either replaced with nullptr or the handler stops.
    */
    struct IBookWriterSink {
        virtual ~IBookWriterSink() = default;

        /**
         * @brief Baseline L2 book for a new lineage (Top-K per side).
         * @details Called once per lineage before any deltas for that lineage.
         *          Snapshot vectors are pre-sorted and contain strictly positive sizes.
         *          Typical action: replace the instrument’s in-memory book state.
         */
        virtual void on_snapshot(const OrderBookSnapshot& s) = 0;

        /**
         * @brief Incremental price-level change (UPSERT/DELETE) applied on the snapshot.
         * @details Deltas arrive in-order and gap-free for the current lineage.
         *          Typical action: mutate the per-instrument book (single-writer).
         *          If you detect a violation (e.g., crossed book), mark for resync.
         */
        virtual void on_delta(const OrderBookDelta& d) = 0;

        /**
         * @brief Optional trade print (does NOT mutate depth).
         * @details Default no-op. Use for analytics/cold-path (VWAP, flow, bars).
         */
        virtual void on_trade(const TradeTick&) {}
    };

    /**
    * @struct IHealthSink
    * @brief Receives health transitions and heartbeats from a FeedHandler.
    *
    * Threading:
    *  - Invoked on the FeedHandler's I/O thread. **Do not block** or throw.
    *  - Do minimal work (e.g., atomically store latest status, enqueue a light task).
    *
    * Semantics:
    *  - Emitted on state transitions (see @ref FeedHealth) and optionally as a
    *    periodic heartbeat (e.g., ~1s) while the state is unchanged.
    *  - Status is per *handler/venue* (not per instrument). Consumers may derive
    *    stricter policies (e.g., mark venue "untradeable" unless HEALTHY).
    *
    * Payload:
    *  - st.venue   : venue id of the handler reporting.
    *  - st.health  : HEALTHY / DEGRADED / STALE / RESYNCING / DOWN.
    *  - st.reason  : short tag for cause (e.g., "GAP", "STALE", "RECONNECT").
    *                 May be empty on routine heartbeats.
    *
    * Idempotency:
    *  - Heartbeats may repeat the same health value. Sinks should de-duplicate if desired.
    *
    * Example (gating logic):
    * @code
    * void on_feed_status(const FeedStatus& st) override {
    *     last_status_.store(st, std::memory_order_relaxed);
    *     if (st.health == FeedHealth::HEALTHY) {
    *         breaker_.reset(st.venue);
    *     } else {
    *         breaker_.trip(st.venue, st.reason); // stop consuming this venue
    *     }
    * }
    * @endcode
    */
    struct IHealthSink {
        virtual ~IHealthSink() = default;
        virtual void on_feed_status(const FeedStatus& st) = 0;
    };

    /**
     * @struct HandlerStats
     * @brief Lightweight counters/latencies for observability.
     *
     * Intended for snapshots (pull) rather than a high-rate stream.
     * Units are implementation-defined; keep them consistent within a process.
     */
    struct HandlerStats {
        // Traffic
        std::uint64_t frames_in{0};        ///< raw frames/messages from wire
        std::uint64_t bytes_in{0};         ///< uncompressed bytes (if known)
        std::uint64_t snapshots_out{0};    ///< OrderBookSnapshot emitted
        std::uint64_t deltas_out{0};       ///< OrderBookDelta emitted
        std::uint64_t trades_out{0};       ///< TradeTick emitted

        // Reliability
        std::uint64_t reconnects{0};
        std::uint64_t resyncs{0};
        std::uint64_t gaps_detected{0};
        std::uint64_t drops_conflation{0}; ///< number of merged/dropped updates due to conflation/backpressure

        // Timing (nanoseconds)
        std::uint64_t p50_decode_ns{0};
        std::uint64_t p99_decode_ns{0};
        std::uint64_t p50_end_to_end_ns{0}; ///< decode→emit
        std::uint64_t p99_end_to_end_ns{0};
    };

    /**
     * @struct SeqInfo
     * @brief Per-instrument sequencing snapshot for debugging/ops.
     */
    struct SeqInfo {
        InstrumentId  inst{0};
        std::uint64_t last_seq{0};
        std::uint64_t snapshot_ver{0};
        std::uint32_t buffered{0};     ///< # of incrementals buffered awaiting order
        std::int64_t  last_ts_recv_ns{0};
    };

    /**
     * @class IFeedHandler
     * @brief Exchange-agnostic market-data ingress for one venue.
     *
     * Responsibilities:
     *  - Connect WebSocket MD, subscribe to channels/instruments.
     *  - Snapshot→replay→stream handshake to ensure a correct lineage.
     *  - Normalize to ticks/lots; enforce strict seq ordering; detect gaps; resync.
     *  - Emit snapshots/deltas/trades to the BookWriter sink; emit health updates.
     *
     * Threading:
     *  - Methods are non-blocking control-plane calls unless noted.
     *  - Callbacks run on the handler’s I/O thread; do not block them.
     */
    class IFeedHandler {
    public:
        /** @brief Virtual dtor. */
        virtual ~IFeedHandler() = default;

        /** @brief Static venue id. @return VenueId. */
        [[nodiscard]] virtual VenueId     venue_id()   const = 0;
        /** @brief Human-readable venue label. @return name string. */
        [[nodiscard]] virtual std::string venue_name() const = 0;

        /**
         * @brief Initialize from external config (no network I/O).
         * @param cfg Parsed configuration (static + hot sections).
         * @return Status::OK if accepted; otherwise an error.
         */
        [[nodiscard]] virtual Status init(const FeedHandlerConfig& cfg) = 0;

        /**
         * @brief Start network I/O and snapshot→replay handshake (non-blocking).
         * @return Status::OK if the start request was accepted.
         */
        [[nodiscard]] virtual Status start() = 0;

        /** @brief Request graceful shutdown (non-blocking). */
        virtual void   stop()  = 0;

        /** @brief Block until the handler has fully stopped. */
        virtual void   join()  = 0;

        /** @brief Running flag. @return true if started and not yet stopped. */
        [[nodiscard]] virtual bool  is_running() const = 0;
        /** @brief Current high-level state. @return @ref State value. */
        [[nodiscard]] virtual State state()      const = 0;

        /**
         * @brief Set the target for normalized book events.
         * @param sink Non-owning pointer; must remain valid while installed.
         * pass nullptr to uninstall
         */
        virtual void set_book_sink(IBookWriterSink* sink) = 0;
        /**
         * @brief Set the target for health updates/heartbeats.
         * @param sink Non-owning pointer; must remain valid while installed.
         * pass nullptr to uninstall.
         */
        virtual void set_health_sink(IHealthSink* sink)   = 0;

        /**
         * @brief Replace the entire instrument watch list.
         * @param full_set New set of InstrumentIds.
         * @return Status::OK if accepted (unsubscribe removed; subscribe new).
         */
        [[nodiscard]] virtual Status set_instruments(const std::vector<InstrumentId>& full_set) = 0;

        /**
         * @brief Add instruments to the watch list.
         * @param add InstrumentIds to add.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status add_instruments(const std::vector<InstrumentId>& add) = 0;

        /**
         * @brief Remove instruments from the watch list.
         * @param remove InstrumentIds to remove.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status remove_instruments(const std::vector<InstrumentId>& remove) = 0;

        /**
         * @brief Change subscribed channels (e.g., DEPTH_L2 | TRADES).
         * @param channels Bitmask of @ref ChannelBits.
         * @return Status::OK if accepted (may trigger resubscribe).
         */
        [[nodiscard]] virtual Status set_channels(ChannelsMask channels) = 0;

        /**
         * @brief Force snapshot→replay for one instrument (bumps snapshot_ver).
         * @param id InstrumentId.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status request_snapshot(InstrumentId id) = 0;

        /**
         * @brief Force snapshot→replay for all current instruments.
         * @return Status::OK if accepted (rate-limited).
         */
        [[nodiscard]] virtual Status request_snapshot_all() = 0;

        /**
         * @brief Force immediate resync to RESYNCING with a reason.
         * @param id InstrumentId.
         * @param reason Short tag (e.g., "GAP", "STALE").
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status force_resync(InstrumentId id, const std::string& reason) = 0;

        /**
         * @brief Force resync for all instruments with a reason.
         * @param reason Short tag.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status force_resync_all(const std::string& reason) = 0;

        /**
         * @brief Set target Top-K depth per side.
         * @param k Levels per side.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_depth_k(std::uint16_t k) = 0;

        /**
         * @brief Enable/disable conflation and its max window.
         * @param enabled On/off.
         * @param window Max hold time for last-value merge.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_conflation(bool enabled, std::chrono::milliseconds window) = 0;

        /**
         * @brief Update liveness/time thresholds.
         * @param t New timeouts.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_timeouts(const Timeouts& t) = 0;

        /**
         * @brief Update reconnect retry parameters.
         * @param b Backoff config.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_reconnect_backoff(const Backoff& b) = 0;

        /**
         * @brief Update snapshot retry parameters.
         * @param b Backoff config.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_snapshot_backoff(const Backoff& b)  = 0;

        /**
         * @brief Set max capacity for out-of-order smoothing buffer.
         * @param capacity Buffer size (by messages).
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_reorder_buffer(std::size_t capacity) = 0;

        /**
         * @brief Choose snapshot source (REST or WS_SNAPSHOT).
         * @param src Source.
         * @return Status::OK if accepted (may trigger resync).
         */
        [[nodiscard]] virtual Status set_snapshot_source(SnapshotSource src) = 0;

        /**
         * @brief Update normalization filters for one instrument.
         * @param id InstrumentId.
         * @param f New Filters (tick/step/minima).
         * @return Status::OK if accepted (should bump snapshot_ver + resync).
         */
        [[nodiscard]] virtual Status set_filters(InstrumentId id, const Filters& f) = 0;

        /**
         * @brief Bulk filters update (adapter may resync per instrument).
         * @param all Vector of (InstrumentId, Filters).
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_filters_bulk(
            const std::vector<std::pair<InstrumentId, Filters>>& all) = 0;

        /**
         * @brief Enable/disable TLS (usually static; may require reconnect).
         * @param enabled true to enable.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_tls(bool enabled) = 0;

        /**
         * @brief Set SPKI certificate pins (usually static; may require reconnect).
         * @param spki_pins Vector of base64(SPKI) pins.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status set_cert_pins(const std::vector<std::string>& spki_pins) = 0;

        /**
         * @brief Current feed health snapshot.
         * @return FeedStatus (venue-level).
         */
        [[nodiscard]] virtual FeedStatus current_status() const = 0;

        /**
         * @brief Lightweight counters/latencies for observability.
         * @return HandlerStats.
         */
        [[nodiscard]] virtual HandlerStats stats() const = 0;

        /**
         * @brief Per-instrument sequencing snapshot.
         * @param id InstrumentId.
         * @return SeqInfo.
         */
        [[nodiscard]] virtual SeqInfo seq_info(InstrumentId id) const = 0;

        /** @brief Current instrument watch list. @return vector of InstrumentId. */
        [[nodiscard]] virtual std::vector<InstrumentId> instruments() const = 0;

        /** @brief Current subscribed channels. @return ChannelsMask. */
        [[nodiscard]] virtual ChannelsMask channels() const = 0;

        /**
         * @brief Resolve venue symbol → InstrumentId.
         * @param venue_symbol Raw venue symbol (e.g., "BTCUSDT").
         * @return InstrumentId (0 if unknown).
         */
        [[nodiscard]] virtual InstrumentId resolve_symbol(std::string_view venue_symbol) const = 0;

        /**
         * @brief Resolve InstrumentId → venue symbol (if known).
         * @param id InstrumentId.
         * @return Symbol string (empty if unknown).
         */
        [[nodiscard]] virtual std::string symbol_for(InstrumentId id) const = 0;

        /**
         * @brief Tear down and reconnect (guarded by rate limits/backoff).
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status reconnect_now() = 0;

        /**
         * @brief Rotate to next failover WS endpoint.
         * @return Status::OK if accepted.
         */
        [[nodiscard]] virtual Status rotate_endpoint() = 0;
    };
}