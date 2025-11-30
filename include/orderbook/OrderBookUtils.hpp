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

    // Helper: convert "12345.67" -> PriceTicks according to your scheme
    static PriceTicks parse_price_to_ticks(const std::string& s) {
        double px = std::stod(s);
        return static_cast<PriceTicks>(px * 100.0); // example: 1 tick = 0.01
    }

    static QtyLots parse_qty_to_lots(const std::string& s) {
        double q = std::stod(s);
        return static_cast<QtyLots>(q * 1000.0); // example: 1 lot = 0.001
    }

} // namespace md
