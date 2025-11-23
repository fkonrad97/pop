#include "ws_client.hpp"
#include "rest_client.hpp"
#include "abstract/feed_handler.hpp"
#include "venue_util.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>

#include "stream_parser/kucoin_stream_parser.hpp"

using json = nlohmann::json;

namespace md {
    class KucoinFeedHandler final : public IVenueFeedHandler {
    public:
        explicit KucoinFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc),
              ws_(std::make_shared<WsClient>(ioc)),
              rest_(std::make_shared<RestClient>(ioc)),
              parser_(std::make_unique<KucoinStreamParser>()) {
        }

        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load()) return Status::ERROR;
            cfg_ = cfg;

            ws_->set_on_message([this](const std::string &msg) {
                // Use fast KucoinStreamParser (simdjson) instead of nlohmann here
                auto maybe_book = parser_->parse_depth5(msg);
                if (!maybe_book) {
                    // DEBUG: show the raw message when parsing fails
                    std::cout << "[KUCOIN][PARSE_FAIL] msg = " << msg << "\n";
                    return;
                }

                Depth5Book book = std::move(*maybe_book);
                book.receive_ts = std::chrono::system_clock::now();

                // For now: debug print; later: push to central brain / orderbook
                std::cout << "[KUCOIN][BOOK] "
                        << book.symbol << " "
                        << "best_bid=" << book.best_bid()
                        << " best_ask=" << book.best_ask()
                        << "\n";
            });

            ws_->set_on_close([this]() {
                running_.store(false);
                // TODO: health/state hooks if needed
            });

            constexpr auto venue_id = md::venue::VenueId::KUCOIN;
            topic_ = md::venue::make_depth_target(
                venue_id,
                cfg_.symbol, // e.g. "btc-usdt"
                cfg_.target // e.g. "depth" or "depth5"
            );

            return Status::OK;
        }

        Status start() override {
            if (running_.exchange(true)) return Status::ERROR;

            // 1) Call KuCoin bullet-public via REST to get token + endpoint
            rest_->async_post(
                "api.kucoin.com",
                "/api/v1/bullet-public",
                "443",
                "{}", // empty JSON body
                [this](boost::system::error_code ec, const std::string &body) {
                    if (ec) {
                        std::cerr << "[KUCOIN][REST] error: " << ec.message() << "\n";
                        running_.store(false);
                        return;
                    }

                    try {
                        json j = json::parse(body);
                        auto token = j.at("data").at("token").get<std::string>();
                        auto endpoint = j.at("data").at("instanceServers").at(0).at("endpoint").get<std::string>();
                        // endpoint like "wss://ws-api-spot.kucoin.com/"

                        std::string ep = endpoint;
                        if (ep.rfind("wss://", 0) == 0) {
                            ep.erase(0, 6); // strip "wss://"
                        }
                        auto slash_pos = ep.find('/');
                        ws_host_ = (slash_pos == std::string::npos) ? ep : ep.substr(0, slash_pos);
                        std::string path = (slash_pos == std::string::npos) ? "/" : ep.substr(slash_pos);

                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::system_clock::now().time_since_epoch()).count();
                        std::string connect_id = "pop-" + std::to_string(ms);

                        ws_port_ = "443";
                        ws_target_ = path + "?token=" + token + "&connectId=" + connect_id;

                        std::cout << "[KUCOIN][REST] endpoint=" << endpoint << "\n";
                        std::cout << "[KUCOIN][REST] host=" << ws_host_
                                << " target=" << ws_target_ << "\n";

                        // 2) Now we can open the WebSocket
                        do_connect_ws_();
                    } catch (const std::exception &ex) {
                        std::cerr << "[KUCOIN][REST] parse error: " << ex.what() << "\n";
                        running_.store(false);
                    }
                });

            return Status::OK;
        }

        Status stop() override {
            if (!running_.exchange(false)) return Status::DISCONNECTED;
            ws_->close();
            return Status::CLOSED;
        }

        bool is_running() const override { return running_.load(); }

    private:
        void do_connect_ws_() {
            std::cout << "[KUCOIN] Connecting to wss://" << ws_host_ << ":" << ws_port_ << ws_target_ << "\n";
            std::cout << "[KUCOIN] Topic: " << topic_ << "\n";

            ws_->set_on_open([this]() {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                json sub_msg = {
                    {"id", static_cast<std::int64_t>(ms)},
                    {"type", "subscribe"},
                    {"topic", topic_},
                    {"response", true}
                };
                std::cout << "[KUCOIN] Subscribing: " << sub_msg.dump() << "\n";
                ws_->send_text(sub_msg.dump());
            });

            ws_->connect(ws_host_, ws_port_, ws_target_);
        }

        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        std::shared_ptr<RestClient> rest_;
        std::unique_ptr<IStreamParser> parser_;

        FeedHandlerConfig cfg_{};
        std::atomic<bool> running_{false};

        std::string ws_host_ = "ws-api-spot.kucoin.com";
        std::string ws_port_ = "443";
        std::string ws_target_;
        std::string topic_;
    };

    // factory
    std::unique_ptr<IVenueFeedHandler> make_kucoin_feed_handler(boost::asio::io_context &ioc) {
        return std::make_unique<KucoinFeedHandler>(ioc);
    }
} // namespace md
