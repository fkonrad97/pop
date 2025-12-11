
#include "orderbook/NOrderBookController.hpp"

namespace md {
    NOrderBookController::Action NOrderBookController::onSnapshot(const GenericSnapshotFormat &msg) {
        // If you store as numeric ticks:
        std::sort(msg.asks.begin(), msg.asks.end(),
                  [](const auto& x, const auto& y){ return x.priceTick > y.priceTick; });

        return;
    }
}
