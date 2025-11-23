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

    class OkxFeedHandler final : public IVenueFeedHandler {
    public:
        explicit OkxFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc),
              ws_(std::make_shared<WsClient>(ioc)) {}

        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load()) return Status::ERROR;
            cfg_ = cfg;

            // Decide how we map symbol/target for OKX
            const auto venue_id = md::venue::to_venue_id(cfg_.venue_name);

            // For OKX, make_depth_target(...) returns CHANNEL name, not path:
            // "books5", "books-l2-tbt", etc.
            okx_channel_ = md::venue::make_depth_target(
                venue_id,
                cfg_.symbol,   // not used in OKX branch but fine
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
                                         ? "ws.okx.com"
                                         : cfg_.host_name;
            const std::string port = cfg_.port.empty()
                                         ? "443"
                                         : cfg_.port;

            // Public WS endpoint is fixed for OKX
            const std::string path = "/ws/v5/public";

            const auto venue_id = venue::to_venue_id(cfg_.venue_name);

            std::cout << "[OKX] Connecting to wss://" << host << ":" << port << path << "\n";

            ws_->set_on_open([this, venue_id]() {
                // Build subscribe message using channel decided in init()
                json sub_msg = {
                    {"op", "subscribe"},
                    {
                        "args", json::array({
                            {
                                {"channel", okx_channel_},
                                {"instId", venue::map_ws_symbol(venue_id, cfg_.symbol)}
                            }
                        })
                    }
                };
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

        std::string okx_channel_; // now stored as member
    };

    std::unique_ptr<IVenueFeedHandler> make_okx_feed_handler(boost::asio::io_context &ioc) {
        return std::make_unique<OkxFeedHandler>(ioc);
    }

} // namespace md