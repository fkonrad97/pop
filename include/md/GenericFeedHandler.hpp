#pragma once

#include <boost/asio/io_context.hpp>
#include <deque>
#include <memory>
#include <variant>
#include <atomic>
#include <string_view>
#include <cstdint>

#include "abstract/FeedHandler.hpp"
#include "client_connection_handlers/WsClient.hpp"
#include "client_connection_handlers/RestClient.hpp"
#include "orderbook/OrderBookController.hpp"
#include "md/VenueAdapter.hpp"
#include "postprocess/FilePersistSink.hpp"

namespace md {
    class GenericFeedHandler final : public IVenueFeedHandler {
    public:
        explicit GenericFeedHandler(boost::asio::io_context &ioc);

        Status init(const FeedHandlerConfig &cfg) override;

        Status start() override;

        Status stop() override;

        /// private methods:
    private:
        /// Adapter Selection
        using AnyAdapter = std::variant<BinanceAdapter, OKXAdapter, BitgetAdapter, BybitAdapter, KucoinAdapter>;

        static AnyAdapter makeAdapter(VenueId v);

        std::uint64_t ws_seen_ = 0; /// Temporary - delete later

        /// Cold-path resolved runtime (no config reads in hot path):
        struct RuntimeResolved {
            VenueId venue{};
            std::size_t depth{0};

            EndPoint ws;
            EndPoint rest;

            std::string wsSubscribeFrame;
            std::string restSnapshotTarget;

            VenueCaps caps;

            int ws_ping_interval_ms{0};
            int ws_ping_timeout_ms{0};
        };

        /// Sync state machine:
        enum class SyncState : std::uint8_t {
            DISCONNECTED,
            CONNECTING,
            BOOTSTRAPPING,

            WAIT_REST_SNAPSHOT, // RestAnchored: WS open, buffering incrementals, REST snapshot in-flight
            WAIT_WS_SNAPSHOT, // WsAuthoritative: WS open, waiting for WS snapshot
            WAIT_BRIDGE, // RestAnchored: REST snapshot loaded, draining buffer until first “bridging” update applied

            SYNCED
        };

        void connectWS();

        void onWSOpen();

        void onWSMessage(const char *data, std::size_t len);

        void requestSnapshot();

        void onSnapshotResponse(std::string_view body);

        void restartSync();

        void drainBufferedIncrementals();

        void bootstrapWS();

        std::string makeConnectId() const;
        static std::int64_t now_ns_() noexcept;

        void onWSClose_();

        void schedule_ws_reconnect_(std::chrono::milliseconds delay);
        void persist_snapshot_(const GenericSnapshotFormat &snap, std::string_view source);
        void persist_incremental_(const GenericIncrementalFormat &inc, std::string_view source);
        void maybe_persist_book_(std::string_view source);

    private:
        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        std::shared_ptr<RestClient> rest_;

        std::string connect_id_;

        std::unique_ptr<OrderBookController> controller_;
        std::unique_ptr<FilePersistSink> persist_;

        FeedHandlerConfig cfg_; /// DO NOT READ IN HOT PATH
        RuntimeResolved rt_;
        AnyAdapter adapter_;

        std::atomic<bool> running_{false};
        SyncState state_{SyncState::DISCONNECTED};

        /// Incremental buffer during snapshot syncing
        struct BufferedMsg {
            std::string payload;
            std::int64_t recv_ts_ns{0};
        };
        std::deque<BufferedMsg> buffer_; /// later optimize to ring buffer / pooled storage
        std::size_t max_buffer_{10'000};

        boost::asio::steady_timer reconnect_timer_;
        std::uint64_t reconnect_gen_{0};
        bool reconnect_scheduled_{false};
        bool closing_for_restart_{false};
        std::size_t persist_book_every_updates_{0};
        std::size_t persist_book_top_{0};
        std::size_t updates_since_book_persist_{0};
    };
}
