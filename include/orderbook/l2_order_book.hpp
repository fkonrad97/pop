#pragma once

#include <cstddef>
#include <vector>

#include "order_book_utils.hpp"

namespace md {

class L2Book {
public:
    explicit L2Book(std::size_t depth)
        : depth_{depth},
          bids_(depth),
          asks_(depth) {}

    [[nodiscard]] std::size_t depth() const noexcept {
        return depth_;
    }

    // public API to be filled in later:
    // - apply_snapshot(...)
    // - apply_incremental_update(...)
    // - best_bid(), best_ask()
    // - mid(), spread(), etc.

private:
    std::size_t depth_{0};
    std::vector<L2Level> bids_;  // index 0 = best bid
    std::vector<L2Level> asks_;  // index 0 = best ask
};

} // namespace md
