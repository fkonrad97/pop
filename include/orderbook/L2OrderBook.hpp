#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>
#include <cassert>

#include "OrderBookUtils.hpp"

namespace md {
    class L2Book {
    public:
        explicit L2Book(std::size_t depth)
            : depth_{depth},
              bids_(depth),
              asks_(depth) {
            assert(depth_ > 0 && "L2Book depth must be > 0");
        }

        [[nodiscard]] std::size_t depth() const noexcept { return depth_; }

        /// Read-only view of bid levels.
        /// Invariant: index 0 is best bid.
        [[nodiscard]] const std::vector<L2Level> &bids() const noexcept {
            return bids_;
        }

        /// Read-only view of ask levels.
        /// Invariant: index 0 is best ask.
        [[nodiscard]] const std::vector<L2Level> &asks() const noexcept {
            return asks_;
        }

        /// Applies a full-depth snapshot for the given book side.
        ///
        /// Expectations:
        ///   - `levels[0]` is the best price for that side,
        ///     `levels[1]` the next, etc. (already sorted by the feed).
        ///   - The snapshot may contain fewer levels than `depth()`.
        ///
        /// Behavior:
        ///   - Copies up to `min(depth(), levels.size())` entries from `levels`
        ///     into the internal side vector (bids_ or asks_).
        ///   - Any remaining levels (if the snapshot is shallower than `depth()`)
        ///     are reset to empty (`L2Level{}`).
        ///   - Never resizes the underlying vectors, so the invariant
        ///       bids_.size() == asks_.size() == depth()
        ///     is preserved.
        ///
        /// Complexity:
        ///   - O(depth()), no dynamic allocations.
        ///
        /// Notes:
        ///   - After this call, `best_bid()` / `best_ask()` for the given side
        ///     reflect the new snapshot (possibly logically empty if qty_lots == 0).
        void apply_snapshot(BookSide side, std::span<const L2Level> levels) noexcept {
            // Select destination side (bids or asks)
            auto &dst = (side == BookSide::Bid) ? bids_mut() : asks_mut();

            const std::size_t n = std::min(depth_, levels.size());

            // Copy as many levels as both book and snapshot have
            std::copy_n(levels.begin(), n, dst.begin());

            // Zero out remaining levels if snapshot is shallower than depth_
            if (n < depth_) {
                std::fill(dst.begin() + n, dst.end(), L2Level{});
            }

            // During development-time:
            assert(dst.size() == depth_);
        }

        /// Applies a single incremental update for the given side.
        ///
        /// Semantics:
        ///   - If qty <= 0:
        ///       * If a level with the given price exists, it is removed:
        ///         subsequent levels are shifted up, and the last slot is cleared.
        ///       * If no level with this price exists, the update is ignored.
        ///   - If qty > 0:
        ///       * If a level with the given price exists, its quantity is replaced
        ///         (`qty_lots = qty`).
        ///       * If no level with this price exists, a new level is inserted at the
        ///         appropriate sorted position (bids: descending price, asks: ascending),
        ///         shifting worse levels down and dropping the last level if needed.
        ///         If the new price is worse than all existing levels and the book is
        ///         full, the update is ignored.
        ///
        /// Invariants:
        ///   - The side vector (bids_ or asks_) remains sorted by price:
        ///       * Bids: highest price at index 0, decreasing.
        ///       * Asks: lowest price at index 0, increasing.
        ///   - The side vector remains compact:
        ///       * All non-empty levels come first, any empty levels (if any) are at
        ///         the tail.
        ///   - The function never resizes the underlying vectors.
        ///
        /// Complexity:
        ///   - O(depth()) in the worst case (linear search + shift).
        void apply_increment(BookSide side, PriceTicks price, QtyLots qty) noexcept {
            auto &levels = (side == BookSide::Bid) ? bids_mut() : asks_mut();

            const std::size_t n = depth_; // same as levels.size()

            // 1) Search for an existing level with this price
            std::size_t idx = n;
            for (std::size_t i = 0; i < n; ++i) {
                if (!levels[i].empty() && levels[i].price_ticks == price) {
                    idx = i;
                    break;
                }
                // stop early if we hit the first empty slot (book is compact)
                if (levels[i].empty()) {
                    break;
                }
            }

            // 2) Deletion / clear case: qty <= 0
            if (qty <= 0) {
                if (idx == n) {
                    // Price not found: nothing to delete
                    return;
                }

                // Shift levels above idx down by one to keep compactness
                for (std::size_t i = idx; i + 1 < n; ++i) {
                    levels[i] = levels[i + 1];
                }
                // Clear the last slot
                levels[n - 1] = L2Level{};
                return;
            }

            // 3) Update case: qty > 0
            if (idx != n) {
                // Existing price level: just update quantity
                levels[idx].qty_lots = qty;
                return;
            }

            // 4) Insert new price level (book may or may not be full)
            // Determine insertion position based on side ordering.
            auto better = [side](PriceTicks lhs, PriceTicks rhs) noexcept {
                if (side == BookSide::Bid) {
                    // Higher price is better for bids
                    return lhs > rhs;
                } else {
                    // Lower price is better for asks
                    return lhs < rhs;
                }
            };

            // Find first position where new price is not "better" than existing level,
            // or the first empty slot.
            std::size_t insert_pos = n;
            for (std::size_t i = 0; i < n; ++i) {
                if (levels[i].empty()) {
                    insert_pos = i;
                    break;
                }
                if (!better(levels[i].price_ticks, price)) {
                    // levels[i] is not better than the new price → insert here
                    insert_pos = i;
                    break;
                }
            }

            if (insert_pos == n) {
                // Book is full and new price is strictly worse than all existing levels:
                // drop this update.
                return;
            }

            // Shift worse levels down by one (from the tail to insert_pos+1),
            // dropping the worst level if the book is full.
            for (std::size_t i = n - 1; i > insert_pos; --i) {
                levels[i] = levels[i - 1];
            }

            // Place new level at insert_pos
            levels[insert_pos].price_ticks = price;
            levels[insert_pos].qty_lots = qty;
        }


        /// Returns the top-of-book bid level (index 0).
        /// May be logically empty (qty_lots == 0) if there is no active bid.
        [[nodiscard]] const L2Level &best_bid() const noexcept {
            return bids_.front();
        }

        /// Returns the top-of-book ask level (index 0).
        /// May be logically empty (qty_lots == 0) if there is no active ask.
        [[nodiscard]] const L2Level &best_ask() const noexcept {
            return asks_.front();
        }

    private:
        /// Internal mutable access – use only inside book methods.
        std::vector<L2Level> &bids_mut() noexcept { return bids_; }
        std::vector<L2Level> &asks_mut() noexcept { return asks_; }

        // Invariant: bids_.size() == asks_.size() == depth_
        std::size_t depth_{0};
        std::vector<L2Level> bids_;
        std::vector<L2Level> asks_;
    };
} // namespace md
