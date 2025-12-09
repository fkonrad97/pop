#pragma once

#include <vector>
#include <string>
#include <cstdint>

namespace md {
    struct BinanceSnapshot {
        std::uint64_t lastUpdateId;
        std::vector<std::pair<std::string, std::string> > bids; // [price, qty]
        std::vector<std::pair<std::string, std::string> > asks;
    };

    struct BinanceDepthUpdate {
        std::uint64_t U; // first update ID in event
        std::uint64_t u; // last update ID in event
        std::uint64_t pu; // previousLastUpdateId (if you store it)
        std::vector<std::pair<std::string, std::string> > bids;
        std::vector<std::pair<std::string, std::string> > asks;
    };
}
