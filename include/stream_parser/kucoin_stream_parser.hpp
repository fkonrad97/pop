#pragma once

#include "abstract/stream_parser.hpp"   // IStreamParser
#include "order_book.hpp"               // Depth5Book

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <chrono>
#include <cstdlib>
#include <limits>
#include <iostream>   // optional, for debug

namespace md {

    class KucoinStreamParser final : public IStreamParser {
    public:
        KucoinStreamParser()  = default;
        ~KucoinStreamParser() override = default;

        std::optional<Depth5Book>
        parse_depth5(const std::string &raw_json) override {
            using json = nlohmann::json;

            json j = json::parse(raw_json, nullptr, false);
            if (j.is_discarded()) {
                std::cout << "[PARSER] invalid json\n";
                return std::nullopt;
            }

            // --- topic ---
            if (!j.contains("topic") || !j["topic"].is_string()) {
                // std::cout << "[PARSER] no topic\n";
                return std::nullopt;
            }
            const std::string topic = j["topic"].get<std::string>();

            // Only handle level2Depth5 orderbook messages
            const std::string prefix = "/spotMarket/level2Depth5:";
            if (topic.rfind(prefix, 0) != 0) {
                std::cout << "[PARSER] wrong topic: " << topic << "\n";
                return std::nullopt;
            }

            if (!j.contains("data") || !j["data"].is_object()) {
                std::cout << "[PARSER] no data\n";
                return std::nullopt;
            }
            const json &data = j["data"];

            Depth5Book book;
            book.venue = venue::VenueId::KUCOIN;

            // --- symbol from topic: "/spotMarket/level2Depth5:BTC-USDT"
            {
                const auto colon_pos = topic.find(':');
                if (colon_pos != std::string::npos && colon_pos + 1 < topic.size()) {
                    book.symbol = topic.substr(colon_pos + 1);  // "BTC-USDT"
                }
            }

            // --- timestamp in ms ---
            if (data.contains("timestamp") && data["timestamp"].is_number_integer()) {
                const auto ts_ms = data["timestamp"].get<long long>();
                using Clock = std::chrono::system_clock;
                book.exchange_ts =
                    Clock::time_point{std::chrono::milliseconds{ts_ms}};
            }

            // No explicit sequence in this channel → use 0 for now
            book.exchange_seq = 0;

            // --- asks: "asks": [["price","qty"], ...] ---
            if (data.contains("asks") && data["asks"].is_array()) {
                const auto &asks = data["asks"];
                const std::size_t depth =
                    std::min<std::size_t>(asks.size(), Depth5Book::kDepth);

                for (std::size_t i = 0; i < depth; ++i) {
                    const auto &lvl = asks[i];
                    if (!lvl.is_array() || lvl.size() < 2) continue;

                    const std::string px_str  = lvl[0].get<std::string>();
                    const std::string qty_str = lvl[1].get<std::string>();

                    const double px  = std::strtod(px_str.c_str(), nullptr);
                    const double qty = std::strtod(qty_str.c_str(), nullptr);

                    if (px > 0.0 && qty > 0.0) {
                        book.asks[i].price    = px;
                        book.asks[i].quantity = qty;
                    }
                }
            }

            // --- bids: "bids": [["price","qty"], ...] ---
            if (data.contains("bids") && data["bids"].is_array()) {
                const auto &bids = data["bids"];
                const std::size_t depth =
                    std::min<std::size_t>(bids.size(), Depth5Book::kDepth);

                for (std::size_t i = 0; i < depth; ++i) {
                    const auto &lvl = bids[i];
                    if (!lvl.is_array() || lvl.size() < 2) continue;

                    const std::string px_str  = lvl[0].get<std::string>();
                    const std::string qty_str = lvl[1].get<std::string>();

                    const double px  = std::strtod(px_str.c_str(), nullptr);
                    const double qty = std::strtod(qty_str.c_str(), nullptr);

                    if (px > 0.0 && qty > 0.0) {
                        book.bids[i].price    = px;
                        book.bids[i].quantity = qty;
                    }
                }
            }

            return book;
        }
    };

} // namespace md
