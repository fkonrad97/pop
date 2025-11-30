#pragma once

#include "abstract/StreamParser.hpp"
#include "orderbook/L2BookController.hpp"
#include <nlohmann/json.hpp>

namespace md {

    class BinanceL2Parser final : public IStreamParser {
    public:
        explicit BinanceL2Parser(L2BookController& controller)
            : controller_(controller) {}

        void on_l2_snapshot(std::string_view msg) override;
        void on_l2_incremental(std::string_view msg) override;

    private:
        L2BookController& controller_;
    };

} // namespace md