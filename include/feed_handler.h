#pragma once            // https://en.wikipedia.org/wiki/Pragma_once
#include <string>
#include <vector>
#include <chrono>

// market data
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
     *   ChannelsMask mask = DEPTH_L2 | TRADES;
     *   if (mask & DEPTH_L2) { / subscribe depth / }
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
    *   @ref PriceLevel::qty_lots (see @ref Filters).
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
    * - No duplicate `px_ticks` within a side; all `qty_lots > 0`.
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

}