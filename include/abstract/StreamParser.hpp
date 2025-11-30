#pragma once

#include <string_view>

namespace md {

    /// Base interface for all venue-specific L2 stream parsers.
    ///
    /// The feed handler is responsible for:
    ///   - Opening the right WS stream (depth5 snapshot, depth@100ms, etc.).
    ///   - Wiring the raw bytes into the *correct* semantic handler below.
    class IStreamParser {
    public:
        virtual ~IStreamParser() = default;

        /// Handle an L2 *snapshot* message (e.g. depth5 / full depth).
        /// `msg` is the raw payload from the WS frame (JSON, usually).
        virtual void on_l2_snapshot(std::string_view msg) = 0;

        /// Handle an L2 *incremental* (diff) message.
        /// `msg` is the raw payload from the WS frame.
        virtual void on_l2_incremental(std::string_view msg) = 0;
    };

} // namespace md