#pragma once

#include <span>
#include <vector>
#include <iostream>

#include "orderbook/BinanceOrderBookController.hpp"
#include "stream_parser/UpdateTypes.hpp"

namespace md
{
    BinanceOrderBookController::Action BinanceOrderBookController::on_snapshot(const BinanceSnapshot &snap)
    {
        /// Clear the whole book first:
        clear_book();

        // 1) Convert REST snapshot → L2Level vectors
        std::vector<L2Level> bids;
        std::vector<L2Level> asks;
        bids.reserve(snap.bids.size());
        asks.reserve(snap.asks.size());

        for (const auto &[px_str, qty_str] : snap.bids)
        {
            L2Level lvl;
            lvl.price_ticks = parse_price_to_ticks(px_str);
            lvl.qty_lots = parse_qty_to_lots(qty_str);
            bids.push_back(lvl);
        }
        for (const auto &[px_str, qty_str] : snap.asks)
        {
            L2Level lvl;
            lvl.price_ticks = parse_price_to_ticks(px_str);
            lvl.qty_lots = parse_qty_to_lots(qty_str);
            asks.push_back(lvl);
        }

        std::span<const L2Level> bid_span{bids.data(), bids.size()};
        std::span<const L2Level> ask_span{asks.data(), asks.size()};

        // 2) Apply snapshot via *generic* base logic
        //    - sets last_seq_ = lastUpdateId
        //    - sets synced_ = true
        apply_snapshot(bid_span, ask_span, snap.lastUpdateId);

        setSyncState(SyncState::HaveSnapshot);

        // 2) Try to consume buffered updates
        if (!process_buffer_after_snapshot())
        {
            setSyncState(SyncState::Broken);
            return Action::NeedResync;
        }

        setSyncState(SyncState::Synced);
        return Action::None;
    }

    BinanceOrderBookController::Action
    BinanceOrderBookController::on_increment(const BinanceDepthUpdate &msg)
    {
        switch (getSyncState())
        {
        case SyncState::WaitingSnapshot:
            pushToBuffer(msg);
            std::cout << getBuffer().size()
                      << " buffered updates while waiting for snapshot // SyncState::WaitingSnapshot \n";
            return Action::None;

        case SyncState::HaveSnapshot:
            pushToBuffer(msg);
            std::cout << getBuffer().size()
                      << " buffered updates // SyncState::HaveSnapshot \n";
            return Action::None;

        case SyncState::Synced:
            std::cout << getBuffer().size()
                      << " buffered updates // SyncState::Synced \n";
            return apply_incremental_synced(msg);

        case SyncState::Broken:
            pushToBuffer(msg);
            std::cout << getBuffer().size()
                      << " buffered updates // SyncState::Broken \n";
            return Action::NeedResync;
        }

        // keep compilers happy about control paths:
        return Action::None;
    }

    BinanceOrderBookController::Action
    BinanceOrderBookController::apply_incremental_synced(const BinanceDepthUpdate &upd)
    {
        const auto last = getLastSeqID();

        // Binance-compatible condition:
        // Event must "cover" last+1: U <= last+1 <= u
        if (!(upd.U <= last + 1 && upd.u >= last + 1))
        {
            setSyncState(SyncState::Broken);
            pushToBuffer(upd);
            return Action::NeedResync;
        }

        if (!apply_update(upd))
        {
            setSyncState(SyncState::Broken);
            return Action::NeedResync;
        }

        // After applying this event, the new last sequence is upd.u
        setLastSeqID(upd.u);
        return Action::None;
    }

    bool BinanceOrderBookController::process_buffer_after_snapshot()
    {
        // Drop all events where u <= last_update_id_
        while (!getBuffer().empty() && getBuffer().front().u <= getLastSeqID())
        {
            popFromBuffer();
        }

        if (getBuffer().empty())
        {
            return true; // nothing to apply yet
        }

        // First event must satisfy: U <= last_id + 1 <= u
        auto &first = getBuffer().front();
        if (!(first.U <= getLastSeqID() + 1 && first.u >= getLastSeqID() + 1))
        {
            return false;
        }

        // Apply first, but make sure we start at last_update_id_+1 logically
        if (!apply_update(first))
        {
            return false;
        }
        setLastSeqID(first.u);
        popFromBuffer();

        // Apply remaining buffered events, enforcing chaining U == prev_u + 1
        while (!getBuffer().empty())
        {
            auto &evt = getBuffer().front();
            if (evt.U != getLastSeqID() + 1)
            {
                return false;
            }
            if (!apply_update(evt))
            {
                return false;
            }
            setLastSeqID(evt.u);
            popFromBuffer();
        }

        return true;
    }

    bool BinanceOrderBookController::apply_update(const BinanceDepthUpdate &upd)
    {
        // Bids
        for (const auto &lvl : upd.bids)
        {
            apply_increment(BookSide::Bid, parse_price_to_ticks(lvl.first), parse_qty_to_lots(lvl.second), upd.u, upd.pu);
        }

        // Asks
        for (const auto &lvl : upd.asks)
        {
            apply_increment(BookSide::Ask, parse_price_to_ticks(lvl.first), parse_qty_to_lots(lvl.second), upd.u, upd.pu);
        }

        return true;
    }

} // namespace md
