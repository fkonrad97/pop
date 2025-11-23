#pragma once

#include "abstract/stream_parser.hpp"   // IStreamParser
#include "order_book.hpp"               // Depth5Book

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <cstdlib>      // std::strtod
#include <algorithm>    // std::min
#include <chrono>

namespace md {

    /**
     * @class OkxStreamParser
     *
     * @brief Parser for OKX books5 JSON into unified Depth5Book.
     *
     * Typical OKX books5 message:
     *
     * {
     *   "arg": {
     *     "channel": "books5",
     *     "instId": "BTC-USDT"
     *   },
     *   "action": "snapshot", // or "update"
     *   "data": [
     *     {
     *       "asks": [["88514.9","0.1","0","1"], ...],
     *       "bids": [["88514.8","0.2","0","2"], ...],
     *       "ts": "1763925000000",
     *       "checksum": 123456789
     *     }
     *   ]
     * }
     */
    class OkxStreamParser final : public IStreamParser {
    public:
        OkxStreamParser()  = default;
        ~OkxStreamParser() override = default;

        std::optional<Depth5Book>
        parse_depth5(const std::string &raw_json) override {
            using json = nlohmann::json;

            json j = json::parse(raw_json, nullptr, false);
            if (j.is_discarded()) {
                return std::nullopt;
            }

            // --- arg/channel ---
            if (!j.contains("arg") || !j["arg"].is_object()) {
                return std::nullopt;
            }
            const json &arg = j["arg"];

            if (!arg.contains("channel") || !arg["channel"].is_string()) {
                return std::nullopt;
            }
            const std::string channel = arg["channel"].get<std::string>();
            if (channel != "books5") {
                // Not the depth5 channel
                return std::nullopt;
            }

            // data: array with a single book object
            if (!j.contains("data") || !j["data"].is_array() || j["data"].empty()) {
                return std::nullopt;
            }
            const json &d0 = j["data"][0];

            Depth5Book book;
            book.venue = venue::VenueId::OKX;

            // --- symbol from instId ---
            if (arg.contains("instId") && arg["instId"].is_string()) {
                book.symbol = arg["instId"].get<std::string>();  // e.g. "BTC-USDT"
            }

            // --- timestamp (ts, string ms) ---
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

            // --- checksum → use as exchange_seq (optional) ---
            if (d0.contains("checksum") && d0["checksum"].is_number_integer()) {
                book.exchange_seq = d0["checksum"].get<std::uint64_t>();
            } else {
                book.exchange_seq = 0;
            }

            // --- asks: first 2 fields of each level ["px","sz",...] ---
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

            // --- bids: first 2 fields of each level ["px","sz",...] ---
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
