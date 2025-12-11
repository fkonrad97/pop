#include "stream_parser/UpdateTypes.hpp"
#include <nlohmann/json.hpp>
#include "abstract/StreamParser.hpp"
<<<<<<< HEAD
<<<<<<< HEAD
#include "orderbook/NOrderBookController.hpp"
=======
#include "orderbook/OrderBookController.hpp"
>>>>>>> ed07ea0 (Refactor Order Book Implementation)
=======
#include "orderbook/OrderBookController.hpp"
>>>>>>> fe24573 (Refactor Order Book Implementation)

using json = nlohmann::json;

namespace md {
<<<<<<< HEAD
<<<<<<< HEAD
    class BinanceStreamParser final : public StreamParser<GenericSnapshotFormat, BinanceDepthUpdate> {
=======
    class BinanceStreamParser final : public StreamParser<GenericSnapshotFormat, GenericIncrementalFormat> {
>>>>>>> ed07ea0 (Refactor Order Book Implementation)
=======
    class BinanceStreamParser final : public StreamParser<GenericSnapshotFormat, GenericIncrementalFormat> {
>>>>>>> fe24573 (Refactor Order Book Implementation)
    public:
        std::optional<GenericSnapshotFormat> parse_snapshot(std::string_view msg) const override;

        std::optional<GenericIncrementalFormat> parse_incremental(std::string_view msg) const override;
    };
}
