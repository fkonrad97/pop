#pragma once

#include "ws_client.hpp"
#include "rest_client.hpp"
#include "feed_handler.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>

using json = nlohmann::json;

namespace md {

    class BinanceFeedHandler final : public IVenueFeedHandler {
    public:
        explicit BinanceFeedHandler(boost::asio::io_context& ioc)
            : ioc_(ioc), ws_(std::make_shared<WsClient>(ioc)) {}

        Status init(const FeedHandlerConfig& cfg) override {
            if (running_.load()) return Status::ERROR;
            cfg_ = cfg;
            // Bind WS callbacks
            ws_->set_on_message([this](const std::string& msg) {
                json jsonObj;
                std::stringstream(msg) >> jsonObj;
                std::cout << jsonObj << "\n";
                // auto h = std::hash<json>{}(jsonObj["asks"]);
                // std::cout << h << "\n";
                std::cout << "\n";
            });
            ws_->set_on_close([this]() {
                running_.store(false);
                // TODO: health/state notify if needed
            });
            return Status::OK;
        }

        Status start() override {
            if (running_.exchange(true)) return Status::ERROR; // already running
            const std::string target_extended = "/ws/" + cfg_.symbol + "@" + cfg_.target;
            ws_->connect(cfg_.host_name, cfg_.port, target_extended);
            return Status::OK;
        }

        void stop() override {
            if (!running_.exchange(false)) return;
            ws_->close();
        }

        bool is_running() const override { return running_.load(); }

    private:
        using Clock = std::chrono::steady_clock;

        boost::asio::io_context& ioc_;
        std::shared_ptr<WsClient> ws_;
        FeedHandlerConfig cfg_{};
        std::atomic<bool> running_{false};
        Clock::time_point last_msg_ts_{};
    };

    // ---- Maker symbol exported for VenueFactory
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context& ioc) {
        return std::make_unique<BinanceFeedHandler>(ioc);
    }

} // namespace md