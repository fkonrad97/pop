#pragma once
#include "../../include/feed_handler.hpp"
#include <gmock/gmock.h>

namespace md {

/// @brief GoogleMock test double for IFeedHandler (use in unit/integration tests).
///        Method semantics follow IFeedHandler docs (lifecycle, sinks, config, etc.).
class MockFeedHandler : public IFeedHandler {
public:
    /// @brief Static venue id (e.g., 1 = BINANCE).
    MOCK_METHOD(VenueId, venue_id, (), (const, override));
    /// @brief Human-readable venue label (for logs/metrics).
    MOCK_METHOD(std::string, venue_name, (), (const, override));

    /// @brief Load static + hot config; no network I/O; returns immediate acceptance code.
    MOCK_METHOD(Status, init, (const FeedHandlerConfig&), (override));
    /// @brief Begin network I/O and snapshot→replay handshake (non-blocking).
    MOCK_METHOD(Status, start, (), (override));
    /// @brief Request graceful stop (non-blocking).
    MOCK_METHOD(void, stop, (), (override));
    /// @brief Block until fully stopped.
    MOCK_METHOD(void, join, (), (override));
    /// @brief True if started and not yet stopped.
    MOCK_METHOD(bool, is_running, (), (const, override));
    /// @brief High-level state snapshot (DISCONNECTED/CONNECTING/.../STREAMING).
    MOCK_METHOD(State, state, (), (const, override));

    /// @brief Install BookWriter sink (snapshots/deltas/trades); nullptr to uninstall.
    MOCK_METHOD(void, set_book_sink, (IBookWriterSink*), (override));
    /// @brief Install Health sink (health transitions/heartbeats); nullptr to uninstall.
    MOCK_METHOD(void, set_health_sink, (IHealthSink*), (override));

    /// @brief Replace entire instrument watch list; unsubscribe removed, subscribe new.
    MOCK_METHOD(Status, set_instruments, (const std::vector<InstrumentId>&), (override));
    /// @brief Add instruments to watch list.
    MOCK_METHOD(Status, add_instruments, (const std::vector<InstrumentId>&), (override));
    /// @brief Remove instruments from watch list.
    MOCK_METHOD(Status, remove_instruments, (const std::vector<InstrumentId>&), (override));

    /// @brief Change subscribed channels bitmask (e.g., DEPTH_L2 | TRADES).
    MOCK_METHOD(Status, set_channels, (ChannelsMask), (override));
    /// @brief Force snapshot→replay for one instrument (bumps snapshot_ver).
    MOCK_METHOD(Status, request_snapshot, (InstrumentId), (override));
    /// @brief Force snapshot→replay for all current instruments.
    MOCK_METHOD(Status, request_snapshot_all, (), (override));
    /// @brief Force immediate resync for one instrument with reason tag.
    MOCK_METHOD(Status, force_resync, (InstrumentId, const std::string&), (override));
    /// @brief Force resync for all instruments with reason tag.
    MOCK_METHOD(Status, force_resync_all, (const std::string&), (override));
    /// @brief Set target Top-K depth per side.
    MOCK_METHOD(Status, set_depth_k, (std::uint16_t), (override));
    /// @brief Enable/disable conflation and set max hold window.
    MOCK_METHOD(Status, set_conflation, (bool, std::chrono::milliseconds), (override));
    /// @brief Update liveness/time thresholds (connect/subscribe/heartbeat/read-idle).
    MOCK_METHOD(Status, set_timeouts, (const Timeouts&), (override));
    /// @brief Update reconnect retry backoff parameters.
    MOCK_METHOD(Status, set_reconnect_backoff, (const Backoff&), (override));
    /// @brief Update snapshot retry backoff parameters.
    MOCK_METHOD(Status, set_snapshot_backoff, (const Backoff&), (override));
    /// @brief Set capacity for out-of-order smoothing buffer.
    MOCK_METHOD(Status, set_reorder_buffer, (std::size_t), (override));
    /// @brief Choose snapshot source (REST or WS_SNAPSHOT); may trigger resync.
    MOCK_METHOD(Status, set_snapshot_source, (SnapshotSource), (override));
    /// @brief Update normalization filters (ticks/lots/minima) for one instrument; should resync.
    MOCK_METHOD(Status, set_filters, (InstrumentId, const Filters&), (override));
    /// @brief Bulk update of normalization filters; implementation may resync per instrument.
    MOCK_METHOD(Status, set_filters_bulk,
                ((const std::vector<std::pair<InstrumentId, Filters>>&)), (override)); // note double parens
    /// @brief Enable/disable TLS (usually static; may require reconnect).
    MOCK_METHOD(Status, set_tls, (bool), (override));
    /// @brief Set SPKI certificate pins (usually static; may require reconnect).
    MOCK_METHOD(Status, set_cert_pins, (const std::vector<std::string>&), (override));

    /// @brief Current venue-level feed health snapshot.
    MOCK_METHOD(FeedStatus, current_status, (), (const, override));
    /// @brief Lightweight counters/latencies snapshot.
    MOCK_METHOD(HandlerStats, stats, (), (const, override));
    /// @brief Per-instrument sequencing snapshot (last seq, snapshot_ver, buffered, timestamps).
    MOCK_METHOD(SeqInfo, seq_info, (InstrumentId), (const, override));

    /// @brief Current instrument watch list.
    MOCK_METHOD(std::vector<InstrumentId>, instruments, (), (const, override));
    /// @brief Current channels bitmask.
    MOCK_METHOD(ChannelsMask, channels, (), (const, override));
    /// @brief Resolve venue symbol → InstrumentId (0 if unknown).
    MOCK_METHOD(InstrumentId, resolve_symbol, (std::string_view), (const, override));
    /// @brief Resolve InstrumentId → venue symbol (empty if unknown).
    MOCK_METHOD(std::string, symbol_for, (InstrumentId), (const, override));

    /// @brief Tear down and reconnect now (guarded by rate limits/backoff).
    MOCK_METHOD(Status, reconnect_now, (), (override));
    /// @brief Rotate to next failover WebSocket endpoint.
    MOCK_METHOD(Status, rotate_endpoint, (), (override));
};

}
