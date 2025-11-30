#pragma once

#include <cstdint>

namespace md {

    // ---- Core scalar types ----
    using PriceTicks = std::int64_t;  // integer price in ticks (smallest price unit)
    using QtyLots    = std::int64_t;  // integer quantity in lots/contracts

    enum class BookSide : std::uint8_t {
        Bid = 0,
        Ask = 1
    };

    // ---- One level in the book ----
    struct L2Level {
        PriceTicks price_ticks {0};
        QtyLots    qty_lots    {0};   // Invariant: qty_lots >= 0 for snapshots

        [[nodiscard]] bool empty() const noexcept {
            return qty_lots == 0;
        }
    };

} // namespace md
