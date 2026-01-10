#pragma once

#include <cstdint>
#include <string>
#include <cmath>

namespace md {
    // Helper: convert "12345.67" -> PriceTicks according to your scheme
    static std::int64_t parsePriceToTicks(const std::string &s) {
        return static_cast<std::int64_t>(std::llround(std::stod(s) * 100.0));
    }

    static std::int64_t parseQtyToLots(const std::string &s) {
        return static_cast<std::int64_t>(std::llround(std::stod(s) * 1000.0));
    }
} // namespace md
