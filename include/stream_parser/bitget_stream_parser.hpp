#pragma once

#include "abstract/stream_parser.hpp"   // IStreamParser
#include "order_book.hpp"               // Depth5Book

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <cstdlib>      // std::strtod, std::strtoll
#include <algorithm>    // std::min
#include <chrono>

namespace md {

    class BitgetStreamParser final : public IStreamParser {
    public:
        BitgetStreamParser()  = default;
        ~BitgetStreamParser() override = default;

        std::optional<Depth5Book>
        parse_depth5(const std::string &raw_json) override {
            using json = nlohmann::json;

            json j = json::parse(raw_json, nullptr, false);
            if (j.is_discarded()) {
                return std::nullopt;
            }

            // --- arg / channel ---
            if (!j.contains("arg") || !j["arg"].is_object()) {
                return std::nullopt;
            }
            const json &arg = j["arg"];

            if (!arg.contains("channel") || !arg["channel"].is_string()) {
                return std::nullopt;
            }
            const std::string channel = arg["channel"].get<std::string>();

            // We only handle books5 here (top 5 levels)
            if (channel != "books5") {
                return std::nullopt;
            }

            // data: array, first element is the book snapshot
            if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
                return std::nullopt;
            }
            const json &d0 = j["data"][0];

            Depth5Book book;
            book.venue = VenueId::BITGET;

            // --- symbol from instId (e.g. "BTCUSDT") ---
            if (arg.contains("instId") && arg["instId"].is_string()) {
                book.symbol = arg["instId"].get<std::string>();
            }

            // --- exchange timestamp from d0.ts (string ms) ---
            if (d0.contains("ts") && d0["ts"].is_string()) {
                const std::string ts_str = d0["ts"].get<std::string>();
                char *end = nullptr;
                long long ts_ms = std::strtoll(ts_str.c_str(), &end, 10);
                if (end != ts_str.c_str()) {
                    using Clock = std::chrono::system_clock;
                    book.exchange_ts =
                        Clock::time_point{std::chrono::milliseconds{ts_ms}};
                }
            }

            // --- seq → use as exchange_seq (monotonic) ---
            if (d0.contains("seq") && d0["seq"].is_number_integer()) {
                book.exchange_seq = d0["seq"].get<std::uint64_t>();
            } else {
                book.exchange_seq = 0;
            }

            // --- asks: [["price","size"], ...] ---
            if (d0.contains("asks") && d0["asks"].is_array()) {
                const auto &asks = d0["asks"];
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

            // --- bids: [["price","size"], ...] ---
            if (d0.contains("bids") && d0["bids"].is_array()) {
                const auto &bids = d0["bids"];
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