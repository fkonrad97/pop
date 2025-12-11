#pragma once

#include <cstdint>

namespace md {
    // Helper: convert "12345.67" -> PriceTicks according to your scheme
    static std::int64_t parsePriceToTicks(const std::string& s) {
        double px = std::stod(s);
        return static_cast<std::int64_t>(px * 100.0); // example: 1 tick = 0.01
    }

    static std::int64_t parseQtyToLots(const std::string& s) {
        double q = std::stod(s);
        return static_cast<std::int64_t>(q * 1000.0); // example: 1 lot = 0.001
    }

} // namespace md
