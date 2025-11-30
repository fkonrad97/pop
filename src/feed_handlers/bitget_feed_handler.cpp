#pragma once

#include "ws_client.hpp"
#include "rest_client.hpp"
#include "abstract/feed_handler.hpp"
#include "venue_util.hpp"

#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>

#include "stream_parser/bitget_stream_parser.hpp"

using json = nlohmann::json;

namespace md {
    class IStreamParser;

    class BitgetFeedHandler final : public IVenueFeedHandler {
    public:
        explicit BitgetFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc),
              ws_(std::make_shared<WsClient>(ioc)),
              parser_(std::make_unique<BitgetStreamParser>()) {
        }

        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load()) return Status::ERROR;
            cfg_ = cfg;

            bitget_channel_ = venue::resolve_stream_channel(*this, cfg_);

            // Bind WS callbacks
            ws_->set_on_raw_message(
                [this](const char *data, std::size_t len)
                {
                    // turn bytes -> std::string for printing
                    std::string msg(data, len);
                    std::cout << "[BITGET RAW] " << msg << "\n";
                });

            ws_->set_on_close([this]() {
                running_.store(false);
                // TODO: health/state hooks if needed
            });

            return Status::OK;
        }

        Status start() override {
            if (running_.exchange(true)) return Status::ERROR;

            const std::string host = cfg_.ws_host.empty()
                                         ? "ws.bitget.com"
                                         : cfg_.ws_host;
            const std::string port = cfg_.ws_port.empty()
                                         ? "443"
                                         : cfg_.ws_port;
            const std::string path = cfg_.ws_path.empty()
                                         ? "/v2/ws/public"
                                         : cfg_.ws_path;
                                         
            std::cout << "[BITGET] Connecting to wss://" << host << ":" << port << path << "\n";
            std::cout << "[BITGET] Subscribing to channel: " << bitget_channel_ << "\n";

            ws_->set_on_open([this]() {
                nlohmann::json sub_msg = {
                    {"op", "subscribe"},
                    {
                        "args", nlohmann::json::array({
                            {
                                {"instType", "SPOT"},
                                {"channel", bitget_channel_}, // e.g. "books5"
                                {"instId", cfg_.symbol} // "BTCUSDT"
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

        std::string incrementalChannelResolver() override { return "books"; }

        /**
         * @brief Map a logical depth spec to an Bitget channel name.
         * https://www.bitget.com/api-doc/spot/websocket/public/Depth-Channel
         *
         * Input example:
         *   "depth5"              -> "books5"
         */
        std::string depthChannelResolver() override {
            switch (cfg_.depthLevel) {
                case 5: {
                    return "books5";
                }
                default: throw std::invalid_argument("Invalid depth level");
            }
        }

    private:
        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        std::unique_ptr<IStreamParser> parser_;
        FeedHandlerConfig cfg_{};
        std::atomic<bool> running_{false};

        std::string bitget_channel_;
    };

    std::unique_ptr<IVenueFeedHandler> make_bitget_feed_handler(boost::asio::io_context &ioc) {
        return std::make_unique<BitgetFeedHandler>(ioc);
    }
} //  namespace md
