
#include "orderbook/OrderBookController.hpp"
#include <iostream>

namespace md
{
    OrderBookController::Action OrderBookController::onSnapshot(const GenericSnapshotFormat &msg)
    {
        /// 0. Reset the whole book first and set the last updated  id
        resetBook();
        setAppliedSeqID(msg.lastUpdateId);

        std::vector<Level> bids = msg.bids;
        std::vector<Level> asks = msg.asks;

        /// 1) Asks sorted by price levels
        std::sort(asks.begin(), asks.end(),
                  [](const auto &x, const auto &y)
                  { return x.priceTick < y.priceTick; });

        /// 2) Bids sorted by price levels
        std::sort(bids.begin(), bids.end(),
                  [](const auto &x, const auto &y)
                  { return x.priceTick > y.priceTick; });

        /// 3) Update internal book
        for (const Level &lvl : bids)
            book_.update<Side::BID>(lvl);
        for (const Level &lvl : asks)
            book_.update<Side::ASK>(lvl);

        /// 4) After applied the snapshot, the SyncState becomes HaveSnapshot
        setSyncState(SyncState::HaveSnapshot);

        // 5) Try to consume buffered updates
        if (!processBuffer())
        {
            setSyncState(SyncState::Broken);
            return Action::NeedResync;
        }

        setSyncState(SyncState::Synced);

        return Action::None;
    }

    OrderBookController::Action OrderBookController::onIncrement(const GenericIncrementalFormat &msg)
    {
        switch (getSyncState())
        {
        case SyncState::WaitingSnapshot:
        {
            pushToBuffer(msg);
            std::cout << bufferSize() << " buffered updates while waiting for snapshot // SyncState::WaitingSnapshot \n";
            return Action::None;
        }

        case SyncState::HaveSnapshot:
        {
            pushToBuffer(msg);
            std::cout << bufferSize() << " buffered updates // SyncState::HaveSnapshot \n";
            return Action::None;
        }

        case SyncState::Synced:
        {
            std::cout << bufferSize()
                      << " buffered updates // SyncState::Synced \n";

            const std::uint64_t applied = getAppliedSeqID();

            // 1) Fully outdated -> ignore
            if (msg.last_seq <= applied)
            {
                return Action::None;
            }

            // 2) If msg starts after the next expected seq -> we missed updates
            const std::uint64_t next_expected = applied + 1;
            if (msg.first_seq > next_expected)
            {
                setSyncState(SyncState::Broken);
                pushToBuffer(msg); // optional: keep it for debugging
                return Action::NeedResync;
            }

            // 3) Normal overlap/cover case: buffer + drain
            pushToBuffer(msg);

            const bool ok = processBuffer();
            if (!ok)
            {
                setSyncState(SyncState::Broken);
                return Action::NeedResync;
            }

            return Action::None;
        }

        case SyncState::Broken:
        {
            pushToBuffer(msg);
            std::cout << bufferSize()
                      << " buffered updates // SyncState::Broken \n";
            return Action::NeedResync;
        }
        }

        return Action::None;
    }

    bool OrderBookController::processBuffer()
    {
        std::uint64_t lastApplied = getAppliedSeqID();

        // 1) Drop fully outdated events (their last_seq is at/before what we already applied)
        while (!buffer_.empty())
        {
            const GenericIncrementalFormat &front = buffer_.front();
            if (front.last_seq <= lastApplied)
            {
                popFromBuffer();
                continue;
            }
            break;
        }

        if (buffer_.empty())
        {
            return true; // nothing to apply
        }

        // 4) Apply events sequentially as long as they keep covering the next expected seq
        while (!buffer_.empty())
        {
            const std::uint64_t required = lastApplied + 1;

            // If this event no longer covers what we need, stop here (gap)
            const GenericIncrementalFormat &ev = buffer_.front();
            if (ev.first_seq > required || ev.last_seq < required)
            {
                return false; // gap detected
            }

            // Apply
            applyIncrementUpdate(ev);

            // Advance last applied sequence and pop
            lastApplied = ev.last_seq;
            setAppliedSeqID(lastApplied);
            popFromBuffer();

            // Drop any newly outdated (due to overlap)
            while (!buffer_.empty() && buffer_.front().last_seq <= lastApplied)
            {
                popFromBuffer();
            }
        }

        return true;
    }

    void OrderBookController::applyIncrementUpdate(const GenericIncrementalFormat &upd)
    {
        for (const Level &lvl : upd.bids)
        {
            book_.update<Side::BID>(lvl);
        }
        for (const Level &lvl : upd.asks)
        {
            book_.update<Side::ASK>(lvl);
        }
    }
}
