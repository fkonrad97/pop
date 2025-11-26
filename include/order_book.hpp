#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <string>

#include "venue_util.hpp"

namespace md {

    using Price    = double;
    using Quantity = double;

    /// Side of the book (if you ever need it separately)
    enum class BookSide : std::uint8_t {
        Bid,
        Ask
    };

    /// Single price level: price + aggregated quantity.
    struct BookLevel {
        Price    price    {0.0};
        Quantity quantity {0.0};
    };

    /// Unified L2 order book snapshot (fixed depth, default=5).
    template<std::size_t Depth = 5>
    struct UnifiedDepthBook {
        static constexpr std::size_t kDepth = Depth;

        // --- Identity ---
        VenueId venue   {};          ///< BINANCE, OKX, ...
        std::string symbol         {};          ///< Normalized symbol, e.g. "BTCUSDT"

        // --- Sequencing ---
        std::uint64_t exchange_seq {0};       ///< Exchange sequence (e.g. lastUpdateId / U/u)
        std::uint64_t local_seq    {0};       ///< Your own monotonic sequence id (optional)

        // --- Timestamps ---
        std::chrono::system_clock::time_point exchange_ts {}; ///< From venue msg if available
        std::chrono::system_clock::time_point receive_ts {};  ///< When your PoP received it

        // --- Book data (0 = best level) ---
        std::array<BookLevel, Depth> bids {}; ///< Best bid at index 0
        std::array<BookLevel, Depth> asks {}; ///< Best ask at index 0

        // --- Convenience helpers ---
        Price best_bid() const noexcept { return bids[0].price; }
        Price best_ask() const noexcept { return asks[0].price; }

        Price mid_price() const noexcept {
            const auto bb = best_bid();
            const auto ba = best_ask();
            return (bb > 0.0 && ba > 0.0) ? (bb + ba) * 0.5 : 0.0;
        }

        bool empty() const noexcept {
            return bids[0].quantity <= 0.0 && asks[0].quantity <= 0.0;
        }
    };

    using Depth5Book = UnifiedDepthBook<5>;

} // namespace md