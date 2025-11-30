#pragma once

#include "ws_client.hpp"
#include "rest_client.hpp"
#include "abstract/feed_handler.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>

#include "venue_util.hpp"
#include "abstract/stream_parser.hpp"
#include "stream_parser/binance_stream_parser.hpp"

using json = nlohmann::json;

namespace md
{

    class BinanceFeedHandler final : public IVenueFeedHandler
    {
    public:
        explicit BinanceFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc), ws_(std::make_shared<WsClient>(ioc)),
              parser_(std::make_unique<BinanceStreamParser>())
        {
        }

        /// 1. IVenueFeedHandler overrides ::
        Status init(const FeedHandlerConfig &cfg) override
        {
            if (running_.load())
                return Status::ERROR;

            cfg_ = cfg;

            // Bind WS callbacks
            ws_->set_on_raw_message(
                [this](const char *data, std::size_t len)
                {
                    // turn bytes -> std::string for printing
                    std::string msg(data, len);
                    std::cout << "[BINANCE RAW] " << data << "\n";
                });

            ws_->set_on_close([this]()
                              {
                                  running_.store(false);
                                  // TODO: health/state notify if needed
                              });
            return Status::OK;
        }

        Status start() override
        {
            if (running_.exchange(true))
                return Status::ERROR; // already running

            const std::string host = cfg_.ws_host.empty()
                                         ? "stream.binance.com"
                                         : cfg_.ws_host;
            const std::string port = cfg_.ws_port.empty()
                                         ? "443"
                                         : cfg_.ws_port;

            const std::string target = venue::resolve_stream_channel(*this, cfg_);

            std::cout << "[BINANCE] Connecting to wss://"
                      << host << ":" << port << "/" << target << "\n";

            ws_->connect(host, port, target);
            return Status::OK;
        }

        Status stop() override
        {
            if (!running_.exchange(false))
                return Status::DISCONNECTED;
            ws_->close();
            return Status::CLOSED;
        }

        bool is_running() const override { return running_.load(); }

        /// 2. IChannelResolver overrides ::
        std::string incrementalChannelResolver() override
        {
            return "@depth";
        }

        /**
         * @brief Map a logical depth spec (e.g. "depth", "depth5@100ms")
         *        to a Binance WS suffix, WITHOUT symbol or "/ws/".
         *
         * Input example:
         *   5        -> "@depth5@100ms"
         */
        std::string depthChannelResolver() override
        {
            std::string prefix = "/ws/" + cfg_.symbol;

            switch (cfg_.depthLevel)
            {
            case 5:
            {
                return prefix + "@depth5";
            }
            default:
                throw std::invalid_argument("Invalid depth level");
            }
        }

    private:
        using Clock = std::chrono::steady_clock;

        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        std::unique_ptr<IStreamParser> parser_;
        FeedHandlerConfig cfg_{};
        std::atomic<bool> running_{false};
        Clock::time_point last_msg_ts_{};
    };

    // ---- Maker symbol exported for VenueFactory
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context &ioc)
    {
        return std::make_unique<BinanceFeedHandler>(ioc);
    }
} // namespace md
