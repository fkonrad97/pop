#pragma once

#include "stream_parser/UpdateTypes.hpp"
#include <vector>
#include "abstract/OrderBookController.hpp"

namespace md {

    class BinanceOrderBookController
        : public OrderBookController<BinanceSnapshot, BinanceDepthUpdate> {
    public:
        using Base = OrderBookController;
        using Base::Base;

        void on_snapshot(const BinanceSnapshot& snap) override;
        void on_increment(const BinanceDepthUpdate& msg) override;

    private:
        enum class SyncState { WaitingSnapshot, Live, Stale };
        SyncState state_{SyncState::WaitingSnapshot};
        std::vector<BinanceDepthUpdate> pre_snapshot_buffer_;
    };

} // namespace md
