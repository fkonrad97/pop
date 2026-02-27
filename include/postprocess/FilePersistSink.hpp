#pragma once

#include <fstream>
#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

#include <nlohmann/json.hpp>

#include "orderbook/OrderBookController.hpp"

namespace md {
    class FilePersistSink {
    public:
        FilePersistSink(std::string path, std::string venue, std::string symbol);

        [[nodiscard]] bool is_open() const noexcept { return out_.is_open(); }

        void write_snapshot(const GenericSnapshotFormat &snap, std::string_view source) noexcept;
        void write_incremental(const GenericIncrementalFormat &inc, std::string_view source) noexcept;
        void write_book_state(const OrderBook &book,
                              std::uint64_t applied_seq,
                              std::size_t top_n,
                              std::string_view source,
                              std::int64_t ts_book_ns) noexcept;

    private:
        static std::int64_t now_ns_() noexcept;
        static nlohmann::json levels_to_json_(const std::vector<Level> &levels);
        static nlohmann::json levels_from_book_(const OrderBook &book, std::size_t top_n, Side side);
        void write_line_(const nlohmann::json &j) noexcept;

    private:
        std::ofstream out_;
        std::string path_;
        std::string venue_;
        std::string symbol_;
        std::uint64_t persist_seq_{0};
    };
}
