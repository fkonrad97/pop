#pragma once

#include <nlohmann/json.hpp>

namespace md {
    /// Base interface for all venue-specific L2 stream parsers.
    ///
    /// The feed handler is responsible for:
    ///   - Opening the right WS stream (depth5 snapshot, depth@100ms, etc.).
    ///   - Wiring the raw bytes into the *correct* semantic handler below.
    template<typename SnapshotMsg, typename IncrementMsg>
    struct StreamParser {
        virtual ~StreamParser() = default;

        /// Handle an L2 *snapshot* message (e.g. depth5 / full depth).
        /// `msg` is the raw payload from the WS frame (JSON, usually).
        virtual std::optional<SnapshotMsg> parse_snapshot(std::string_view msg) const = 0;

        /// Handle an L2 *incremental* (diff) message.
        /// `msg` is the raw payload from the WS frame.
        virtual std::optional<IncrementMsg> parse_incremental(std::string_view msg) const = 0;
    };
} // namespace md
