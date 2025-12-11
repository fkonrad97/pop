#include "stream_parser/BinanceStreamParser.hpp"
#include "stream_parser/UpdateTypes.hpp"
#include <nlohmann/json.hpp>
#include "orderbook/OrderBookUtils.hpp"

#include "orderbook/NParentOrderBookController.hpp"

using json = nlohmann::json;

namespace md {
    std::optional<GenericSnapshotFormat> BinanceStreamParser::parse_snapshot(std::string_view msg) const {
        try {
            json j = json::parse(msg);

            GenericSnapshotFormat snap;
            snap.lastUpdateId = j["lastUpdateId"].get<std::uint64_t>();

            for (const auto &b: j["bids"]) {
                const auto &px_str = b[0].get_ref<const std::string &>();
                const auto &qty_str = b[1].get_ref<const std::string &>();
                snap.bids.push_back(Level{ md::parsePriceToTicks(px_str), md::parseQtyToLots(qty_str) });
            }

            for (const auto &a: j["asks"]) {
                const auto &px_str = a[0].get_ref<const std::string &>();
                const auto &qty_str = a[1].get_ref<const std::string &>();
                snap.asks.push_back(Level{ md::parsePriceToTicks(px_str), md::parseQtyToLots(qty_str) });
            }

            return snap;
        } catch (...) {
            // log and return std::nullopt
            return std::nullopt;
        }
    }

    std::optional<GenericIncrementalFormat> BinanceStreamParser::parse_incremental(std::string_view msg) const {
        try {
            json j = json::parse(msg);
            if (!j.contains("e") || j["e"] != "depthUpdate") {
                return std::nullopt;
            }

            GenericIncrementalFormat update;
            update.first_seq = j["U"].get<std::uint64_t>();
            update.last_seq = j["u"].get<std::uint64_t>();
            update.prev_last = j.value("pu", update.last_seq - 1);

            for (const auto &b: j["b"]) {
                const auto &px_str = b[0].get_ref<const std::string &>();
                const auto &qty_str = b[1].get_ref<const std::string &>();
                update.bids.push_back(Level{ md::parsePriceToTicks(px_str), md::parseQtyToLots(qty_str) });
            }
            for (const auto &a: j["a"]) {
                const auto &px_str = a[0].get_ref<const std::string &>();
                const auto &qty_str = a[1].get_ref<const std::string &>();
                update.asks.push_back(Level{ md::parsePriceToTicks(px_str), md::parseQtyToLots(qty_str) });
            }

            return update;
        } catch (...) {
            return std::nullopt;
        }
    }
};
