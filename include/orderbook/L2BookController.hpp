#pragma once

#include <cstdint>
#include <vector>

#include "L2OrderBook.hpp"
#include "OrderBookUtils.hpp"

namespace md {
    class L2BookController {
    public:
        explicit L2BookController(std::size_t depth)
            : book_{depth} {
        }

        /// Apply a fresh full snapshot from the venue.
        /// `bids` and `asks` are already sorted (best first).
        /// `last_seq` is the last sequence/update id covered by this snapshot.
        void apply_snapshot(const std::vector<L2Level> &bids,
                            const std::vector<L2Level> &asks,
                            std::uint64_t last_seq);

        /// Apply a single incremental price-level update from the venue.
        /// `side`  : bid or ask
        /// `price` : price in ticks
        /// `qty`   : ABSOLUTE quantity at this price (0 means delete)
        /// `seq`   : sequence/update id of this message
        /// `prev_seq` : previous sequence id (if the venue exposes it; else pass seq-1)
        void apply_increment(BookSide side,
                             PriceTicks price,
                             QtyLots qty,
                             std::uint64_t seq,
                             std::uint64_t prev_seq);

        [[nodiscard]] const L2Book &book() const noexcept { return book_; }

        [[nodiscard]] bool is_synced() const noexcept { return synced_; }

        /// Last sequence id successfully applied to the book.
        [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }
    private:
        L2Book book_;
        bool synced_{false};
        std::uint64_t last_seq_{0};
    };
} // namespace md
