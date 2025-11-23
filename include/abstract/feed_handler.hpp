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
    enum class Status { OK, ERROR, HEALTHY, DEGRADED, DISCONNECTED, RESYNCED, SYNCHING, CLOSED };

    /**
     * @brief Minimal configuration for a venue feed.
     *
     * Extend options:
     *  - host, port, ws_target (e.g., "/ws/btcusdt@aggTrade")
     *  - multiple symbols/channels (orderbook, trades, tickers)
     *  - timeouts, heartbeat, backoff policy
     */
    struct FeedHandlerConfig {
        std::string venue_name;  ///< e.g. "BINANCE", "OKX"
        std::string symbol;      ///< Logical symbol, e.g. "BTCUSDT"
        std::string host_name;   ///< Optional override. If empty, venue default is used.
        std::string port;        ///< Optional override. If empty, venue default is used.

        /**
         * @brief Logical stream / channel descriptor.
         *
         * Examples:
         *   - "depth"           → venue-specific default depth stream
         *   - "depth5@100ms"    → Binance: depth 5 @ 100ms
         *   - "trades"          → trade stream
         *
         * Each venue is responsible for mapping this logical string to:
         *   - actual WS URL path (e.g. "/ws/btcusdt@depth5@100ms")
         *   - or WS channel name + fixed URL (OKX: "/ws/v5/public" + {"channel": "..."}).
         */
        std::string target;
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
        virtual Status init(const FeedHandlerConfig&) = 0;

        /// Enqueue the async network chain on the (externally-driven) io_context.
        virtual Status start() = 0;

        /// Gracefully stop: close sockets and cancel timers. Safe to call multiple times.
        virtual Status stop() = 0;

        virtual bool is_running() const = 0;
    };

} // namespace md
