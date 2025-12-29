#pragma once

#include <boost/asio/io_context.hpp>  /// External event loop
#include <string>

namespace md {
    /**
     * @brief Return code for feed operations.
     *
     * Semantics:
     *  - OK    : Operation was accepted and enqueued (for async ops), or completed successfully (for sync ops).
     *  - ERROR : Precondition failed (e.g., already started), invalid config, or immediate failure to enqueue.
     *
     * Note: For async chains, detailed errors should be reported via logs/callbacks, not just this enum.
     */
    enum class Status {
        OK,
        ERROR,
        HEALTHY,
        DEGRADED,
        DISCONNECTED,
        RESYNCED,
        SYNCHING,
        CLOSED
    };

    enum class VenueId { BINANCE, OKX, BYBIT, BITGET, KUCOIN, UNKNOWN };

    inline const char *to_string(VenueId k) {
        switch (k) {
            case VenueId::BINANCE: return "binance";
            case VenueId::OKX: return "okx";
            case VenueId::BYBIT: return "bybit";
            case VenueId::BITGET: return "bitget";
            case VenueId::KUCOIN: return "kucoin";
            default: return "UNKNOWN";
        }
    }

    /**
     * @brief Minimal configuration for a venue feed.
     *
     * Extend options:
     *  - host, port, ws_target (e.g., "/ws/btcusdt@aggTrade")
     *  - multiple symbols/channels (orderbook, trades, tickers)
     *  - timeouts, heartbeat, backoff policy
     */
    struct FeedHandlerConfig {
        VenueId venue_name; ///< e.g. VenueId::BINANCE

        std::string symbol; ///< Symbol normalized to venue requirements
        std::string base_ccy; ///< e.g. "BTC"
        std::string quote_ccy; ///< e.g. "USDT"

        std::string ws_host; ///< optional override, "" = default
        std::string ws_port; ///< optional override, "" = default
        std::string ws_path; ///< optional override, "" = default

        std::string rest_host; ///< optional override, "" = default
        std::string rest_port; ///< optional override, "" = default
        std::string rest_path; ///< optional override, "" = default

        size_t depthLevel{0};
    };

    /**
     * @brief Abstract interface for a venue-specific feed handler.
     *
     * Lifecycle (single-threaded strand suggested):
     *   1) init(cfg)   : validate config, capture references (ioc, clients), prep subscriptions. Non-blocking preferred.
     *   2) start()     : enqueue async ops on the provided io_context (e.g., resolve→connect→handshake→subscribe).
     *   3) stop()      : close sockets / cancel timers. Idempotent and safe at any time.
     *
     * Contract:
     *   - init(...) must be called exactly once before start().
     *   - start() may be called once; repeated calls should return ERROR or no-op safely.
     *   - stop() is idempotent; it must not throw; it should cause on-close/health updates as appropriate.
     */
    struct IVenueFeedHandler {
        virtual ~IVenueFeedHandler() = default;

        /// Prepare resources and validate configuration. Should NOT block on network.
        virtual Status init(const FeedHandlerConfig &) = 0;

        /// Enqueue the async network chain on the (externally-driven) io_context.
        virtual Status start() = 0;

        /// Gracefully stop: close sockets and cancel timers. Safe to call multiple times.
        virtual Status stop() = 0;
    };
} // namespace md
