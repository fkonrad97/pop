//
// Created by Konrád Farkas on 2025. 11. 24..
//

#ifndef BINANCE_JSON_PARSER_HPP
#define BINANCE_JSON_PARSER_HPP

#endif //BINANCE_JSON_PARSER_HPP
#pragma once

#include "abstract/stream_parser.hpp"   // IStreamParser
#include "order_book.hpp"               // Depth5Book (kDepth, best_bid/best_ask, etc.)

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <cstdlib>      // std::strtod
#include <algorithm>    // std::min

namespace md {

    /**
     * @class BinanceStreamParser
     *
     * @brief Parser for Binance depth5 JSON into unified Depth5Book.
     *
     * Example payload (btcusdt@depth5@100ms):
     *
     * {
     *   "lastUpdateId": 81707209471,
     *   "bids": [
     *     ["88514.88000000","3.13481000"],
     *     ...
     *   ],
     *   "asks": [
     *     ["88514.89000000","0.43417000"],
     *     ...
     *   ]
     * }
     *
     * Notes:
     *  - Payload does NOT contain the symbol; we inject it via the constructor.
     *  - No exchange timestamp here; feed handler should set receive_ts.
     */
    class BinanceStreamParser final : public IStreamParser {
    public:
        explicit BinanceStreamParser(std::string symbol = {})
            : symbol_(std::move(symbol)) {}

        ~BinanceStreamParser() override = default;

        std::optional<Depth5Book>
        parse_depth5(const std::string &raw_json) override {
            using json = nlohmann::json;

            json j = json::parse(raw_json, nullptr, false);
            if (j.is_discarded()) {
                return std::nullopt;
            }

            if (!j.contains("bids") || !j["bids"].is_array() ||
                !j.contains("asks") || !j["asks"].is_array()) {
                // Not a depth snapshot/update we care about
                return std::nullopt;
            }

            Depth5Book book;
            book.venue = VenueId::BINANCE;

            // Symbol is not in the message → use ctor-injected symbol if present
            if (!symbol_.empty()) {
                book.symbol = symbol_;
            }

            // Optional: lastUpdateId → exchange_seq
            if (j.contains("lastUpdateId") && j["lastUpdateId"].is_number_unsigned()) {
                book.exchange_seq = j["lastUpdateId"].get<std::uint64_t>();
            }

            // Binance depth5 has no exchange timestamp here:
            // - leave book.exchange_ts as default
            // - feed handler should set book.receive_ts = now

            // --- bids ---
            {
                const auto &bids = j["bids"];
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

            // --- asks ---
            {
                const auto &asks = j["asks"];
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

    private:
        std::string symbol_;   // e.g. "BTCUSDT" or "BTC-USDT" depending on your convention
    };

} // namespace md