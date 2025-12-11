#pragma once

#include "stream_parser/UpdateTypes.hpp"
#include <vector>
#include "abstract/OrderBookController.hpp"

namespace md
{

    class BinanceOrderBookController
        : public OrderBookController<BinanceSnapshot, BinanceDepthUpdate>
    {
    public:
        using Base = OrderBookController;
        using Base::Base;

        Action on_snapshot(const BinanceSnapshot &snap) override;
        Action on_increment(const BinanceDepthUpdate &msg) override;

        bool process_buffer_after_snapshot();
        bool apply_update(const BinanceDepthUpdate &upd);
        Action apply_incremental_synced(const BinanceDepthUpdate &upd);
    };

} // namespace md
