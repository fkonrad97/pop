#pragma once

#include <optional>
#include <string>

#include "order_book.hpp"

namespace md {

    /// Base interface for all venue-specific JSON parsers.
    class IStreamParser {
    public:
        virtual ~IStreamParser() = default;

        /// Parse an orderbook depth5 message (snapshot or update).
        /// Returns std::nullopt if the message is not a depth message
        /// for this venue or if parsing fails.
        virtual std::optional<Depth5Book> parse_depth5(const std::string& raw_json) = 0;
    };

} // namespace md
