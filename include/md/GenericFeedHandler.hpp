#pragma once

#include <boost/asio/io_context.hpp>
#include <deque>
#include <memory>
#include <variant>
#include <atomic>
#include <string_view>

#include "abstract/FeedHandler.hpp"
#include "client_connection_handlers/WsClient.hpp"
#include "client_connection_handlers/RestClient.hpp"
#include "orderbook/OrderBookController.hpp"
#include "md/VenueAdapter.hpp"

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

        void onWSClose_();

        void schedule_ws_reconnect_(std::chrono::milliseconds delay);

    private:
        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        std::shared_ptr<RestClient> rest_;

        std::string connect_id_;

        std::unique_ptr<OrderBookController> controller_;

        FeedHandlerConfig cfg_; /// DO NOT READ IN HOT PATH
        RuntimeResolved rt_;
        AnyAdapter adapter_;

        std::atomic<bool> running_{false};
        SyncState state_{SyncState::DISCONNECTED};

        /// Incremental buffer during snapshot syncing
        std::deque<std::string> buffer_; /// later optimize to ring buffer / pooled storage
        std::size_t max_buffer_{10'000};

        boost::asio::steady_timer reconnect_timer_;
        std::uint64_t reconnect_gen_{0};
        bool reconnect_scheduled_{false};
        bool closing_for_restart_{false};
    };
}
