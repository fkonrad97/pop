#pragma once

#include "ws_client.hpp"
#include "rest_client.hpp"
#include "feed_handler.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>

#include "venue_util.hpp"

using json = nlohmann::json;

namespace md {
    /**
    * WS: @depth
    {
       "e": "depthUpdate", // Event type
        "E": 123456789,     // Event time
        "T": 123456788,     // Transaction time
        "s": "BTCUSDT",     // Symbol
        "U": 157,           // First update ID in event
        "u": 160,           // Final update ID in event
        "pu": 149,          // Final update Id in last stream(ie `u` in last stream)
        "b": [              // Bids to be updated
            [
            "0.0024",       // Price level to be updated
            "10"            // Quantity
            ]
        ],
        "a": [              // Asks to be updated
            [
            "0.0026",       // Price level to be updated
            "100"          // Quantity
            ]
        ]
    }
    */

    /**
     * WS: depth@<level>
    {
        "e": "depthUpdate", // Event type
        "E": 1571889248277, // Event time
        "T": 1571889248276, // Transaction time
        "s": "BTCUSDT",
        "U": 390497796,     // First update ID in event
        "u": 390497878,     // Final update ID in event
        "pu": 390497794,    // Final update Id in last stream(ie `u` in last stream)
        "b": [              // Bids to be updated
        [
            "7403.89",      // Price Level to be updated
            "0.002"         // Quantity
        ],
        [
            "7403.90",
            "3.906"
        ],
            ...
        ],
        "a": [              // Asks to be updated
        [
            "7405.96",      // Price level to be
            "3.340"         // Quantity
        ],
        [
            "7406.63",
            "4.525"
        ],
            ...
        ]
    }
    */

    /** Example: @depth5
        {"lastUpdateId":79544937539,"bids":[["110209.81000000","5.55324000"], ...],"asks":[["110209.82000000","2.45228000"], ...]}
     */

    /** Example: @depth
        {"e":"depthUpdate","E":1762107420814,"s":"BTCUSDT","U":79544963298,"u":79544963298,"b":[["110050.00000000","0.02040000"]],"a":[]}
     */
    class BinanceFeedHandler final : public IVenueFeedHandler {
    public:
        explicit BinanceFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc), ws_(std::make_shared<WsClient>(ioc)) {
        }

        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load()) return Status::ERROR;

            cfg_ = cfg;

            // Bind WS callbacks
            ws_->set_on_message([this](const std::string &msg) {
                json jsonObj;
                std::stringstream(msg) >> jsonObj;
                std::cout << msg << "\n";
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

            const std::string host = cfg_.host_name.empty()
                                         ? "stream.binance.com"
                                         : cfg_.host_name;
            const std::string port = cfg_.port.empty()
                                         ? "443"
                                         : cfg_.port;

            const auto venue_id = venue::to_venue_id(cfg_.venue_name);

            // For BINANCE: make_depth_target → "/ws/btcusdt@depth5@100ms"
            const std::string target = md::venue::make_depth_target(
                venue_id,
                cfg_.symbol, // e.g. "btcusdt"
                cfg_.target // e.g. "depth5@100ms" or "depth"
            );

            std::cout<<"String target :: " << std::endl<<target<<std::endl;

            std::cout << "[BINANCE] Connecting to wss://"
                    << host << ":" << port << "/" << target << "\n";

            ws_->connect(host, port, target);
            return Status::OK;
        }

        Status stop() override {
            if (!running_.exchange(false)) return Status::DISCONNECTED;
            ws_->close();
            return Status::CLOSED;
        }

        bool is_running() const override { return running_.load(); }

    private:
        using Clock = std::chrono::steady_clock;

        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        FeedHandlerConfig cfg_{};
        std::atomic<bool> running_{false};
        Clock::time_point last_msg_ts_{};
    };

    // ---- Maker symbol exported for VenueFactory
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context &ioc) {
        return std::make_unique<BinanceFeedHandler>(ioc);
    }
} // namespace md
