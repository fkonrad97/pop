#include "orderbook/L2BookController.hpp"

#include <span>   // std::span

namespace md {

    void L2BookController::apply_snapshot(const std::vector<L2Level> &bids,
                                          const std::vector<L2Level> &asks,
                                          std::uint64_t last_seq) {
        // Adapt std::vector -> std::span for the book API
        std::span bid_span{bids.data(), bids.size()};
        std::span ask_span{asks.data(), asks.size()};

        // Reset internal book from snapshot
        book_.apply_snapshot(BookSide::Bid, bid_span);
        book_.apply_snapshot(BookSide::Ask, ask_span);

        // Update sequence tracking
        last_seq_ = last_seq;
        synced_   = true;
    }

    void L2BookController::apply_increment(BookSide side,
                                           PriceTicks price,
                                           QtyLots qty,
                                           std::uint64_t seq,
                                           std::uint64_t prev_seq) {
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

} // namespace md