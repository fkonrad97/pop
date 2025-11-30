#pragma once

#include "orderbook/OrderBook.hpp"
#include "orderbook/OrderBookUtils.hpp"
#include <span>

namespace md {
    template<typename SnapshotMsg, typename IncrementMsg>
    class OrderBookController {
    public:
        explicit OrderBookController(std::size_t depth)
            : book_{depth} {
        }

        virtual ~OrderBookController() = default;

        /// Forced venue-specific hooks:
        virtual void on_snapshot(const SnapshotMsg &msg) = 0;

        virtual void on_increment(const IncrementMsg &msg) = 0;

        [[nodiscard]] const OrderBook &book() const noexcept { return book_; }
        [[nodiscard]] bool is_synced() const noexcept { return synced_; }
        /// Last sequence id successfully applied to the book.
        [[nodiscard]] std::uint64_t last_seq() const noexcept { return last_seq_; }

    protected:
        /// Apply a fresh full snapshot from the venue.
        /// `bids` and `asks` are already sorted (best first).
        /// `last_seq` is the last sequence/update id covered by this snapshot.
        void apply_snapshot(std::span<const L2Level> bids,
                            std::span<const L2Level> asks,
                            std::uint64_t last_seq) noexcept {
            // Adapt std::vector -> std::span for the book API
            std::span bid_span{bids.data(), bids.size()};
            std::span ask_span{asks.data(), asks.size()};

            // Reset internal book from snapshot
            book_.apply_snapshot(BookSide::Bid, bid_span);
            book_.apply_snapshot(BookSide::Ask, ask_span);

            // Update sequence tracking
            last_seq_ = last_seq;
            synced_ = true;
        }

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
                             std::uint64_t prev_seq) noexcept {
            // If we are not synced, ignore incrementals until a fresh snapshot arrives.
            if (!synced_) {
                return;
            }

            // Simple gap check:
            //
            // Expectations on the caller:
            //  - prev_seq is the sequence id that this update logically follows.
            //  - For venues that don't expose "prev_seq", you can pass last_seq_.
            //
            // If there is a mismatch, we consider the book stale and stop applying
            // incrementals until a new snapshot is applied.
            if (last_seq_ != 0 && prev_seq != last_seq_) {
                // Gap or out-of-order update detected -> mark as unsynced.
                synced_ = false;
                return;
            }

            // Sequence is consistent -> apply update into the book.
            book_.apply_increment(side, price, qty);

            // Advance last_seq_ to this update's sequence id.
            last_seq_ = seq;
        }

    private:
        OrderBook book_;
        bool synced_{false};
        std::uint64_t last_seq_{0};
    };
} // namespace md
