#pragma once

#include "ws_client.hpp"
#include "rest_client.hpp"
#include "feed_handler.hpp"
#include "venue_util.hpp"

#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>
#include <sstream>

using json = nlohmann::json;

namespace md {

    class ByBitFeedHandler final : public IVenueFeedHandler {
    public:
        explicit ByBitFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc),
              ws_(std::make_shared<WsClient>(ioc)) {}

        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load()) return Status::ERROR;
            cfg_ = cfg;

            const auto venue_id = venue::to_venue_id(cfg_.venue_name);

            bybit_channel_ = venue::make_depth_target(
                venue_id,
                cfg_.symbol,
                cfg_.target
            );

            ws_->set_on_message([this](const std::string &msg) {
                json jsonObj;
                std::stringstream(msg) >> jsonObj;
                std::cout << msg << "\n\n";
                // TODO: route to orderbook instead of printing
            });

            ws_->set_on_close([this]() {
                running_.store(false);
                // TODO: health/state hooks if needed
            });

            return Status::OK;
        }

        Status start() override {
            if (running_.exchange(true)) return Status::ERROR;

            const std::string host = cfg_.host_name.empty()
                                   ? "stream.bybit.com"
                                   : cfg_.host_name;
            const std::string port = cfg_.port.empty()
                                   ? "443"
                                   : cfg_.port;

            // For now: spot public WS (can be parameterized later)
            const std::string path = "/v5/public/spot";

            std::cout << "[BYBIT] Connecting to wss://" << host << ":" << port << path << "\n";
            std::cout << "[BYBIT] Subscribing to channel: " << bybit_channel_ << "\n";

            ws_->set_on_open([this]() {
                // Example JSON: {"op":"subscribe","args":["orderbook.5.BTCUSDT"]}
                nlohmann::json sub_msg = {
                    {"op", "subscribe"},
                    {"type", "snapshot"},
                    {"args", nlohmann::json::array({ bybit_channel_ })}
                };
                std::cout<<sub_msg.dump()<<"\n";
                ws_->send_text(sub_msg.dump());
            });

            ws_->connect(host, port, path);
            return Status::OK;
        }

        Status stop() override {
            if (!running_.exchange(false)) return Status::DISCONNECTED;
            ws_->close();
            return Status::CLOSED;
        }

        bool is_running() const override { return running_.load(); }

    private:
        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        FeedHandlerConfig cfg_{};
        std::atomic<bool> running_{false};

        std::string bybit_channel_; // now stored as member
    };

    std::unique_ptr<IVenueFeedHandler> make_bybit_feed_handler(boost::asio::io_context &ioc) {
        return std::make_unique<ByBitFeedHandler>(ioc);
    }

} //  namespace md