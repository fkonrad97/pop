#pragma once

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "connection_handler/WsClient.hpp"
#include "orderbook/OrderBookController.hpp"

namespace md {
    // Publishes normalized feed events to a central "brain" via websocket.
    // Payloads intentionally match the JSONL objects written by FilePersistSink
    // (except they are sent as individual WS text messages).
    class WsPublishSink {
    public:
        WsPublishSink(boost::asio::io_context &ioc,
                      std::string host,
                      std::string port,
                      std::string target,
                      bool insecure_tls,
                      std::string venue,
                      std::string symbol);

        void start();
        void stop();

        void publish_snapshot(const GenericSnapshotFormat &snap, std::string_view source) noexcept;
        void publish_incremental(const GenericIncrementalFormat &inc, std::string_view source) noexcept;
        void publish_book_state(const OrderBook &book,
                                std::uint64_t applied_seq,
                                std::size_t top_n,
                                std::string_view source,
                                std::int64_t ts_book_ns) noexcept;

    private:
        static std::int64_t now_ns_() noexcept;
        static nlohmann::json levels_to_json_(const std::vector<Level> &levels);
        static nlohmann::json levels_from_book_(const OrderBook &book, std::size_t top_n, Side side);

        void connect_();
        void schedule_reconnect_();

        void send_json_(nlohmann::json &j) noexcept;

    private:
        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        boost::asio::steady_timer reconnect_timer_;

        std::string host_;
        std::string port_;
        std::string target_;
        std::string venue_;
        std::string symbol_;

        bool running_{false};
        bool reconnect_scheduled_{false};
        int reconnect_delay_ms_{1000};
        std::uint64_t reconnect_gen_{0};

        std::uint64_t persist_seq_{0};
    };
} // namespace md
