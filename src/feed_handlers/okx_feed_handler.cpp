#pragma once

#include "ws_client.hpp"
#include "rest_client.hpp"
#include "abstract/feed_handler.hpp"
#include "venue_util.hpp"

#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>
#include <sstream>

#include "stream_parser/okx_stream_parser.hpp"

using json = nlohmann::json;

namespace md {
    class OkxFeedHandler final : public IVenueFeedHandler {
    public:
        explicit OkxFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc),
              ws_(std::make_shared<WsClient>(ioc)),
              parser_(std::make_unique<OkxStreamParser>()) {
        }

        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load()) return Status::ERROR;
            cfg_ = cfg;

            // For OKX, make_depth_target(...) returns CHANNEL name, not path:
            // "books5", "books-l2-tbt", etc.
            okx_channel_ = venue::resolve_stream_channel(*this, cfg_);

            ws_->set_on_message([this](const std::string &msg) {
                auto maybe_book = parser_->parse_depth5(msg);
                if (!maybe_book) {
                    // DEBUG: show the raw message when parsing fails
                    std::cout << "[OKX][PARSE_FAIL] msg = " << msg << "\n";
                    return;
                }

                Depth5Book book = std::move(*maybe_book);
                book.receive_ts = std::chrono::system_clock::now();

                // For now: debug print; later: push to central brain / orderbook
                std::cout << "[OKX][BOOK] "
                        << book.symbol << " "
                        << "best_bid=" << book.best_bid()
                        << " best_ask=" << book.best_ask()
                        << "\n";
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

            std::cout << "[OKX] Connecting to wss://" << host << ":" << port << path << "\n";

            ws_->set_on_open([this]() {
                // Build subscribe message using channel decided in init()
                json sub_msg = {
                    {"op", "subscribe"},
                    {
                        "args", json::array({
                            {
                                {"channel", okx_channel_},
                                {"instId", cfg_.symbol}
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

        std::string incrementalChannelResolver() override { return "books";}
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

        std::string okx_channel_; // now stored as member
    };

    std::unique_ptr<IVenueFeedHandler> make_okx_feed_handler(boost::asio::io_context &ioc) {
        return std::make_unique<OkxFeedHandler>(ioc);
    }
} // namespace md
