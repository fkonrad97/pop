#pragma once

#include "abstract/stream_parser.hpp"   // IStreamParser
#include "order_book.hpp"               // Depth5Book

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <cstdlib>      // std::strtod
#include <algorithm>    // std::min
#include <chrono>

namespace md {

    class BybitStreamParser final : public IStreamParser {
    public:
        BybitStreamParser()  = default;
        ~BybitStreamParser() override = default;

        std::optional<Depth5Book>
        parse_depth5(const std::string &raw_json) override {
            using json = nlohmann::json;

            json j = json::parse(raw_json, nullptr, false);
            if (j.is_discarded()) {
                return std::nullopt;
            }

            // --- topic / type ---
            if (!j.contains("topic") || !j["topic"].is_string()) {
                return std::nullopt;
            }
            const std::string topic = j["topic"].get<std::string>();

            // Expect orderbook.* topics
            if (topic.rfind("orderbook.", 0) != 0) {
                return std::nullopt;
            }

            if (!j.contains("type") || !j["type"].is_string()) {
                return std::nullopt;
            }
            const std::string type = j["type"].get<std::string>();

            // We only treat snapshots as full 5-level books here
            if (type != "snapshot") {
                return std::nullopt;
            }

            if (!j.contains("data") || !j["data"].is_object()) {
                return std::nullopt;
            }
            const json &data = j["data"];

            Depth5Book book;
            book.venue = VenueId::BYBIT;

            // --- symbol ---
            if (data.contains("s") && data["s"].is_string()) {
                book.symbol = data["s"].get<std::string>(); // e.g. "BTCUSDT"
            }

            // --- sequence: prefer seq, fallback to u ---
            if (data.contains("seq") && data["seq"].is_number_unsigned()) {
                book.exchange_seq = data["seq"].get<std::uint64_t>();
            } else if (data.contains("u") && data["u"].is_number_unsigned()) {
                book.exchange_seq = data["u"].get<std::uint64_t>();
            } else {
                book.exchange_seq = 0;
            }

            // --- exchange timestamp: use cts (engine ts) if present, else ts ---
            using Clock = std::chrono::system_clock;

            if (j.contains("cts") && j["cts"].is_number_integer()) {
                long long ts_ms = j["cts"].get<long long>();
                book.exchange_ts = Clock::time_point{std::chrono::milliseconds{ts_ms}};
            } else if (j.contains("ts") && j["ts"].is_number_integer()) {
                long long ts_ms = j["ts"].get<long long>();
                book.exchange_ts = Clock::time_point{std::chrono::milliseconds{ts_ms}};
            }

            // --- bids: [["price","size"], ...] ---
            if (data.contains("b") && data["b"].is_array()) {
                const auto &bids = data["b"];
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

            // --- asks: [["price","size"], ...] ---
            if (data.contains("a") && data["a"].is_array()) {
                const auto &asks = data["a"];
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

            return book;
        }
    };

} // namespace md
