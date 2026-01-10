#include "orderbook/OrderBookController.hpp"

#include <algorithm>
#include <iostream>

namespace md {
    OrderBookController::Action
    OrderBookController::onSnapshot(const GenericSnapshotFormat &msg, BaselineKind kind) {
        resetBook();

        std::vector<Level> bids = msg.bids;
        std::vector<Level> asks = msg.asks;

        std::sort(asks.begin(), asks.end(),
                  [](const Level &x, const Level &y) { return x.priceTick < y.priceTick; });

        std::sort(bids.begin(), bids.end(),
                  [](const Level &x, const Level &y) { return x.priceTick > y.priceTick; });

        for (const Level &lvl: bids) book_.update<Side::BID>(lvl);
        for (const Level &lvl: asks) book_.update<Side::ASK>(lvl);

        last_seq_ = msg.lastUpdateId;
        expected_seq_ = last_seq_ + 1;

        const bool checksum_enabled = (checksum_fn_ != nullptr);

        // If checksum is enabled, require it to be present and correct.
        if (checksum_enabled) {
            if (msg.checksum == 0) {
                resetBook();
                return Action::NeedResync;
            }
            if (!validateChecksum(msg.checksum)) {
                resetBook();
                return Action::NeedResync;
            }
        }

        state_ = (kind == BaselineKind::WsAuthoritative)
                     ? SyncState::Synced
                     : SyncState::WaitingBridge;

        return Action::None;
    }

    OrderBookController::Action
    OrderBookController::onIncrement(const GenericIncrementalFormat &msg) {
        if (state_ == SyncState::WaitingSnapshot) {
            return Action::None; // handler buffers
        }

        const bool has_seq = (msg.last_seq != 0);
        const bool checksum_enabled = (checksum_fn_ != nullptr);

        if (!has_seq && !checksum_enabled) {
            return Action::NeedResync;
        }

        // ---- Bridging phase (RestAnchored) ----
        if (state_ == SyncState::WaitingBridge) {
            if (has_seq) {
                const std::uint64_t required = expected_seq_;

                std::cerr << "[CTRL][BRIDGE] last_seq_=" << last_seq_
                        << " required=" << required
                        << " msg.first=" << msg.first_seq
                        << " msg.last=" << msg.last_seq
                        << "\n";

                if (msg.last_seq < required) {
                    std::cerr << "[CTRL][BRIDGE] IGNORE too-old: msg.last < required ("
                            << msg.last_seq << " < " << required << ")\n";
                    return Action::None;
                }

                if (msg.first_seq > required) {
                    std::cerr << "[CTRL][BRIDGE] RESYNC gap: msg.first > required ("
                            << msg.first_seq << " > " << required << ")\n";
                    return Action::NeedResync;
                }

                // Covers required (overlap OK for absolute level-set updates)
                std::cerr << "[CTRL][BRIDGE] APPLY covers required=" << required
                        << " (first=" << msg.first_seq << ", last=" << msg.last_seq << ")\n";

                applyIncrementUpdate(msg);
                last_seq_ = msg.last_seq;
                expected_seq_ = last_seq_ + 1;
                state_ = SyncState::Synced;

                std::cerr << "[CTRL][BRIDGE] -> Synced last_seq_=" << last_seq_
                        << " expected_seq_=" << expected_seq_ << "\n";
            } else {
                std::cerr << "[CTRL][BRIDGE] APPLY seq-less -> Synced\n";
                applyIncrementUpdate(msg);
                state_ = SyncState::Synced;
            }

            if (checksum_enabled) {
                if (msg.checksum == 0) {
                    std::cerr << "[CTRL][BRIDGE] RESYNC missing checksum while enabled\n";
                    resetBook();
                    return Action::NeedResync;
                }
                if (!validateChecksum(msg.checksum)) {
                    std::cerr << "[CTRL][BRIDGE] RESYNC checksum mismatch\n";
                    resetBook();
                    return Action::NeedResync;
                }
            }

            return Action::None;
        }

        // ---- Steady-state (Synced) ----
        if (has_seq) {
            const std::uint64_t required = expected_seq_;

            if (msg.last_seq < required) return Action::None; // outdated
            if (msg.first_seq > required) return Action::NeedResync; // gap

            // overlap/cover is OK
            applyIncrementUpdate(msg);
            last_seq_ = msg.last_seq;
            expected_seq_ = last_seq_ + 1;
        } else {
            // seq-less venue: checksum is the integrity guard
            applyIncrementUpdate(msg);
        }

        if (checksum_enabled) {
            if (msg.checksum == 0) {
                resetBook();
                return Action::NeedResync;
            }
            if (!validateChecksum(msg.checksum)) {
                resetBook();
                return Action::NeedResync;
            }
        }

        return Action::None;
    }

    void OrderBookController::applyIncrementUpdate(const GenericIncrementalFormat &upd) {
        for (const Level &lvl: upd.bids) book_.update<Side::BID>(lvl);
        for (const Level &lvl: upd.asks) book_.update<Side::ASK>(lvl);
    }
} // namespace md
