#include "stream_parser/BinanceStreamParser.hpp"
#include "stream_parser/UpdateTypes.hpp"
#include <nlohmann/json.hpp>

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
                snap.bids.emplace_back(px_str, qty_str);
            }

            for (const auto &a: j["asks"]) {
                const auto &px_str = a[0].get_ref<const std::string &>();
                const auto &qty_str = a[1].get_ref<const std::string &>();
                snap.asks.emplace_back(px_str, qty_str);
            }

            return snap;
        } catch (...) {
            // log and return std::nullopt
            return std::nullopt;
        }
    }

    std::optional<BinanceDepthUpdate> BinanceStreamParser::parse_incremental(std::string_view msg) const {
        try {
            json j = json::parse(msg);
            if (!j.contains("e") || j["e"] != "depthUpdate") {
                return std::nullopt;
            }

            BinanceDepthUpdate upd;
            upd.U = j["U"].get<std::uint64_t>();
            upd.u = j["u"].get<std::uint64_t>();
            upd.pu = j.value("pu", upd.u - 1);

            for (const auto &b: j["b"]) {
                const auto &px_str = b[0].get_ref<const std::string &>();
                const auto &qty_str = b[1].get_ref<const std::string &>();
                upd.bids.emplace_back(px_str, qty_str);
            }
            for (const auto &a: j["a"]) {
                const auto &px_str = a[0].get_ref<const std::string &>();
                const auto &qty_str = a[1].get_ref<const std::string &>();
                upd.asks.emplace_back(px_str, qty_str);
            }

            return upd;
        } catch (...) {
            return std::nullopt;
        }
    }
};
