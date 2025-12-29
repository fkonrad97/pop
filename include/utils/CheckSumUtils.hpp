#pragma once

#include <string_view>
#include <boost/crc.hpp>
#include <cstddef>
#include <cstdint>

namespace md {
    class OrderBook;

    using ChecksumFn = bool(*)(const OrderBook &book,
                               std::int64_t expected_checksum,
                               std::size_t topN) noexcept;

    inline std::uint32_t CRC32Checksum(std::string_view s) {
        boost::crc_32_type result;
        result.process_bytes(s.data(), s.size());
        return result.checksum();
    }

    inline std::int64_t CRC32ToSigned(std::uint32_t u) noexcept {
        return static_cast<std::int32_t>(u); // preserve bit pattern
    }

    /// Need read access to top levels.
    /// returning nullptr if i out of range / empty.
    inline bool checkBitgetCRC32(const OrderBook &book,
                                 std::int64_t expected,
                                 std::size_t topN) noexcept {
        std::string s;
        s.reserve(topN * 64);

        bool first = true;
        auto append = [&](const std::string &tok) {
            if (!first) s.push_back(':');
            first = false;
            s.append(tok);
        };

        for (std::size_t i = 0; i < topN; ++i) {
            if (const Level *b = book.bid_ptr(i)) {
                append(b->price);
                append(b->quantity);
            }
            if (const Level *a = book.ask_ptr(i)) {
                append(a->price);
                append(a->quantity);
            }
        }

        const std::uint32_t u = CRC32Checksum(s);
        const std::int64_t local = CRC32ToSigned(u);
        return local == expected;
    }
}
