#include "stream_parser/UpdateTypes.hpp"
#include <nlohmann/json.hpp>
#include "abstract/StreamParser.hpp"
#include "orderbook/OrderBookController.hpp"

using json = nlohmann::json;

namespace md {
    class BinanceStreamParser final : public StreamParser<GenericSnapshotFormat, GenericIncrementalFormat> {
    public:
        std::optional<GenericSnapshotFormat> parse_snapshot(std::string_view msg) const override;

        std::optional<GenericIncrementalFormat> parse_incremental(std::string_view msg) const override;
    };
}
