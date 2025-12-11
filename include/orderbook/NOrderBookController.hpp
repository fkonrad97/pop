#pragma once

#include <vector>

#include "NOrderBook.hpp"

struct GenericSnapshotFormat {
    std::uint64_t lastUpdateId;
    std::vector<std::pair<std::string, std::string>> bids; // [price, qty]
    std::vector<std::pair<std::string, std::string>> asks;
};

struct GenericIncrementalFormat {

};

namespace md {
    class NOrderBookController {
    public:
        explicit NOrderBookController(const std::size_t depth): book_{depth} {}

        virtual ~NOrderBookController() = default;

        enum class Action
        {
            None,      // all good
            NeedResync // detected gap, caller should re-request snapshot
        };

        enum class SyncState
        {
            WaitingSnapshot,
            HaveSnapshot,
            Synced,
            Broken
        };

        virtual Action onSnapshot(const GenericSnapshotFormat &msg) = 0;

    private:
        NOrderBook book_;
    };


}