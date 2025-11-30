#include "stream_parser/UpdateTypes.hpp"
#include <nlohmann/json.hpp>
#include "abstract/StreamParser.hpp"

using json = nlohmann::json;

namespace md {
    class BinanceStreamParser final : public StreamParser<BinanceSnapshot, BinanceDepthUpdate> {
    public:
        std::optional<BinanceSnapshot> parse_snapshot(std::string_view msg) const override;

        std::optional<BinanceDepthUpdate> parse_incremental(std::string_view msg) const override;
    };
}
