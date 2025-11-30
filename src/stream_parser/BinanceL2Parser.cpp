#include "stream_parser/BinanceL2Parser.hpp"

#include <iostream>

using json = nlohmann::json;

namespace md {

    // Helper: convert "12345.67" -> PriceTicks according to your scheme
    static PriceTicks parse_price_to_ticks(const std::string& s) {
        double px = std::stod(s);
        return static_cast<PriceTicks>(px * 100.0); // example: 1 tick = 0.01
    }

    static QtyLots parse_qty_to_lots(const std::string& s) {
        double q = std::stod(s);
        return static_cast<QtyLots>(q * 1000.0); // example: 1 lot = 0.001
    }

    void BinanceL2Parser::on_l2_snapshot(std::string_view msg) {
        try {
            json j = json::parse(msg);

            // REST snapshot-like payloads look like:
            // { "lastUpdateId": 123456, "bids": [[ "px","qty" ], ...], "asks": [...] }

            std::uint64_t last_id = j["lastUpdateId"].get<std::uint64_t>();

            std::vector<L2Level> bids;
            std::vector<L2Level> asks;

            bids.reserve(j["bids"].size());
            asks.reserve(j["asks"].size());

            for (const auto& b : j["bids"]) {
                const auto& px_str  = b[0].get_ref<const std::string&>();
                const auto& qty_str = b[1].get_ref<const std::string&>();

                L2Level lvl;
                lvl.price_ticks = parse_price_to_ticks(px_str);
                lvl.qty_lots    = parse_qty_to_lots(qty_str);
                bids.push_back(lvl);
            }

            for (const auto& a : j["asks"]) {
                const auto& px_str  = a[0].get_ref<const std::string&>();
                const auto& qty_str = a[1].get_ref<const std::string&>();

                L2Level lvl;
                lvl.price_ticks = parse_price_to_ticks(px_str);
                lvl.qty_lots    = parse_qty_to_lots(qty_str);
                asks.push_back(lvl);
            }

            controller_.apply_snapshot(bids, asks, last_id);

        } catch (const std::exception& ex) {
            std::cerr << "[BinanceL2Parser] snapshot parse error: " << ex.what() << "\n";
        }
    }

    void BinanceL2Parser::on_l2_incremental(std::string_view msg) {
        try {
            json j = json::parse(msg);

            // depthUpdate:
            // {
            //   "e": "depthUpdate",
            //   "E": 123456789,
            //   "s": "BTCUSDT",
            //   "U": 157,
            //   "u": 160,
            //   "pu": 156,
            //   "b": [ ["price", "qty"], ... ],
            //   "a": [ ["price", "qty"], ... ]
            // }

            if (!j.contains("e") || j["e"] != "depthUpdate") {
                return;
            }

            if (!controller_.is_synced()) {
                // No snapshot yet / resync in progress
                return;
            }

            std::uint64_t U  = j["U"].get<std::uint64_t>();
            std::uint64_t u  = j["u"].get<std::uint64_t>();
            std::uint64_t pu = j.value("pu", u - 1);

            // Bids updates
            for (const auto& b : j["b"]) {
                const auto& px_str  = b[0].get_ref<const std::string&>();
                const auto& qty_str = b[1].get_ref<const std::string&>();

                PriceTicks price_ticks = parse_price_to_ticks(px_str);
                QtyLots    qty_lots    = parse_qty_to_lots(qty_str);

                controller_.apply_increment(BookSide::Bid,
                                            price_ticks,
                                            qty_lots,
                                            u,
                                            pu);
            }

            // Asks updates
            for (const auto& a : j["a"]) {
                const auto& px_str  = a[0].get_ref<const std::string&>();
                const auto& qty_str = a[1].get_ref<const std::string&>();

                PriceTicks price_ticks = parse_price_to_ticks(px_str);
                QtyLots    qty_lots    = parse_qty_to_lots(qty_str);

                controller_.apply_increment(BookSide::Ask,
                                            price_ticks,
                                            qty_lots,
                                            u,
                                            pu);
            }

        } catch (const std::exception& ex) {
            std::cerr << "[BinanceL2Parser] incremental parse error: " << ex.what() << "\n";
        }
    }

} // namespace md