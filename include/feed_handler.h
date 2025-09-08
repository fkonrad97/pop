#pragma once            // https://en.wikipedia.org/wiki/Pragma_once
#include <chrono>

// market data
namespace md {

    // Core IDs
    using VenueId      = std::uint16_t;   // e.g., 1=BINANCE, 2=KRAKEN...
    using InstrumentId = std::uint32_t;   // internal symbol id, e.g., BTCUSDT:1001

    // Status & State
    enum class Status : std::uint8_t {  // return value of a call, a receipt for a request (accepted/rejected)
        OK,
        INVALID_STATE,
        UNKNOWN_SYMBOL,
        NOT_RUNNING,
        BUSY,
        UNSUPPORTED,
        ERROR
    };

    enum class State : std::uint8_t {   // current lifecycle of the handler, what mode the machine is currently in
        DISCONNECTED,
        CONNECTING,
        SUBSCRIBED,
        SNAPSHOTTING,
        REPLAYING,
        STREAMING,
        RESYNCING
    };


    /**
     *  DEPTH_L1: want best bid/ask (top-of-book) updates
     *  DEPTH_L2: want price-level deltas (aggregated sizes)
     *  TRADES:   want last trades (px, qty, taker side)
     */
    enum ChannelBits : std::uint32_t {  // what to subscribe to
        DEPTH_L1 = 1u << 0,  // Top-of-book (best bid/ask)
        DEPTH_L2 = 1u << 1,  // Aggregated depth by price level (Top-K)
        TRADES   = 1u << 2,  // Prints/trade ticks
    };
    using ChannelsMask = std::uint32_t;

    struct Timeouts {
        int connect_ms   {3000};   // Upper bound to complete TCP + TLS + WS upgrade. If exceeded → reconnect with backoff/failover
        int subscribe_ms {3000};   // After sending subscriptions, expect an ACK or first data by this time. If not, retry or resubscribe
        int heartbeat_ms {10000};  // Expected keepalive/heartbeat interval from the venue. If a heartbeat hasnt been seen within ~heartbeat_ms, increment a miss counter
        int read_idle_ms {15000};  // If no data of any kind (payload or heartbeat) for this long, mark STALE and consider resync/reconnect
    };

    struct Backoff {
        int   base_ms {200};   // first retry delay
        int   cap_ms  {2000};  // maximum delay between retries
        double jitter {0.20};  // +/-20% randomization
    };
}