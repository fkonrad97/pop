#pragma once

#include <span>
#include <vector>

#include "orderbook/BinanceOrderBookController.hpp"
#include "stream_parser/UpdateTypes.hpp"

namespace md {
    void BinanceOrderBookController::on_snapshot(const BinanceSnapshot &snap) {
        // 1) Convert REST snapshot → L2Level vectors
        std::vector<L2Level> bids;
        std::vector<L2Level> asks;
        bids.reserve(snap.bids.size());
        asks.reserve(snap.asks.size());

        for (const auto &[px_str, qty_str]: snap.bids) {
            L2Level lvl;
            lvl.price_ticks = parse_price_to_ticks(px_str);
            lvl.qty_lots = parse_qty_to_lots(qty_str);
            bids.push_back(lvl);
        }
        for (const auto &[px_str, qty_str]: snap.asks) {
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

        // 3) Replay buffered WS events to "catch up" with the stream
        if (!pre_snapshot_buffer_.empty()) {
            const std::uint64_t L = snap.lastUpdateId;
            bool started = false;

            for (const auto &ev: pre_snapshot_buffer_) {
                // Binance docs: find first event where
                //   ev.U <= L + 1 && ev.u >= L + 1
                if (!started) {
                    if (ev.U <= L + 1 && ev.u >= L + 1) {
                        started = true;
                    } else {
                        continue; // still before snapshot
                    }
                }

                const std::uint64_t seq = ev.u;
                const std::uint64_t prevSeq = ev.pu; // Binance's previous ID

                for (const auto &[px_str, qty_str]: ev.bids) {
                    apply_increment(BookSide::Bid,
                                    parse_price_to_ticks(px_str),
                                    parse_qty_to_lots(qty_str),
                                    seq,
                                    prevSeq);
                }
                for (const auto &[px_str, qty_str]: ev.asks) {
                    apply_increment(BookSide::Ask,
                                    parse_price_to_ticks(px_str),
                                    parse_qty_to_lots(qty_str),
                                    seq,
                                    prevSeq);
                }

                if (!is_synced()) {
                    // Base detected a gap via prevSeq != last_seq()
                    state_ = SyncState::Stale;
                    break;
                }
            }
        }

        pre_snapshot_buffer_.clear();
        if (is_synced()) {
            state_ = SyncState::Live;
        } else {
            state_ = SyncState::Stale;
        }
    }

    void BinanceOrderBookController::on_increment(const BinanceDepthUpdate &msg) {
        switch (state_) {
            case SyncState::WaitingSnapshot:
                // We haven't got a trusted REST snapshot yet → just buffer WS events.
                pre_snapshot_buffer_.push_back(msg);
                return;

            case SyncState::Stale:
                // We know we're out of sync; ignore updates until caller fetches a new snapshot.
                return;

            case SyncState::Live:
                break; // handle below
        }

        // Live mode: snapshot applied and buffer replay done.
        const std::uint64_t seq = msg.u;
        const std::uint64_t prevSeq = msg.pu;

        for (const auto &[px_str, qty_str]: msg.bids) {
            apply_increment(BookSide::Bid,
                            parse_price_to_ticks(px_str),
                            parse_qty_to_lots(qty_str),
                            seq,
                            prevSeq);
        }
        for (const auto &[px_str, qty_str]: msg.asks) {
            apply_increment(BookSide::Ask,
                            parse_price_to_ticks(px_str),
                            parse_qty_to_lots(qty_str),
                            seq,
                            prevSeq);
        }

        if (!is_synced()) {
            // generic controller detected a gap or out-of-order update
            state_ = SyncState::Stale;
        }
    }
} // namespace md
