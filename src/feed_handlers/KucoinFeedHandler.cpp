#include "client_connection_handlers/WsClient.hpp"
#include "client_connection_handlers/RestClient.hpp"
#include "abstract/FeedHandler.hpp"
#include "VenueUtils.hpp"

#include <nlohmann/json.hpp>
#include <atomic>
#include <iostream>

#include "abstract/StreamParser.hpp"

using json = nlohmann::json;

namespace md {
    class KucoinFeedHandler final : public IVenueFeedHandler {
    public:
        explicit KucoinFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc),
              ws_(std::make_shared<WsClient>(ioc)),
              rest_(std::make_shared<RestClient>(ioc)) {}

        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load()) return Status::ERROR;
            cfg_ = cfg;

            // Bind WS callbacks
            ws_->set_on_raw_message(
                [this](const char *data, std::size_t len)
                {
                    // turn bytes -> std::string for printing
                    std::string msg(data, len);
                    std::cout << "[KUCOIN RAW] " << msg << "\n";
                });

            ws_->set_on_close([this]() {
                running_.store(false);
                // TODO: health/state hooks if needed
            });

            topic_ = venue::resolve_stream_channel(*this, cfg_);

            return Status::OK;
        }

        Status start() override {
            if (running_.exchange(true)) return Status::ERROR;

            // 1) Call KuCoin bullet-public via REST to get token + endpoint
            rest_->async_post(
                cfg_.rest_host.empty() ? "api.kucoin.com" : cfg_.rest_host,
                cfg_.rest_port.empty() ? "443" : cfg_.rest_port,
                cfg_.rest_path.empty() ? "/api/v1/bullet-public" : cfg_.rest_path,
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

        std::string incrementalChannelResolver() override { return "level2"; }

        /**
         * https://www.kucoin.com/docs-new/3470068w0
         */
        std::string depthChannelResolver() override {
            std::string levelChannel = [this]() -> std::string {
                switch (cfg_.depthLevel) {
                    case 5:  return "level2Depth5";
                    default: throw std::invalid_argument("Invalid depth level");
                }
            }();

            return "/spotMarket/" + levelChannel + ":" + cfg_.symbol;
        }

    private:
        void do_connect_ws_() {
            std::cout << "[KUCOIN] Connecting to wss://" << ws_host_ << ":" << ws_port_ << ws_target_ << "\n";
            std::cout << "[KUCOIN] Topic: " << topic_ << "\n";

            ws_->set_on_open([this]() {
                auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                json sub_msg = {
                    {"id", ms},
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
