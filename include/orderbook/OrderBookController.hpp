#pragma once

#include <vector>
#include <deque>
#include "OrderBook.hpp"

struct GenericSnapshotFormat
{
    std::uint64_t lastUpdateId;
    std::vector<Level> bids;
    std::vector<Level> asks;
};

struct GenericIncrementalFormat
{
    std::uint64_t first_seq;                // inclusive
    std::uint64_t last_seq;                 // inclusive (can equal first_seq)
    std::optional<std::uint64_t> prev_last; // if venue provides
    std::vector<Level> bids;
    std::vector<Level> asks;
};

namespace md
{
    class OrderBookController
    {
    public:
        explicit OrderBookController(const std::size_t depth) : book_{depth} {}

        virtual ~OrderBookController() = default;

        enum class Action
        {
            None, // all good
            NeedResync
        };

        enum class SyncState
        {
            WaitingSnapshot,
            HaveSnapshot,
            Synced,
            Broken
        };

        /**
         * 'onSnapshot' process the incoming snapshot message from the exchange
         * @param msg - snapshot message
         */
        Action onSnapshot(const GenericSnapshotFormat &msg);

        /**
         * 'onIncrement' process the incoming incremental update message from the exchange
         * @param msg - incremental update message
         */
        Action onIncrement(const GenericIncrementalFormat &msg);

        void applyIncrementUpdate(const GenericIncrementalFormat &upd);

        void resetBook()
        {
            book_.clear();
            buffer_.clear();
            state_ = SyncState::WaitingSnapshot;
            synced_ = false;
            last_seq_ = 0;
        }

        [[nodiscard]] const OrderBook &book() const noexcept { return book_; }

        /**
         * 'isSynced' indicates whether the order book is currently synchronized with the exchange data feed.
         */
        [[nodiscard]] bool isSynced() const noexcept
        {
            return state_ == SyncState::Synced;
        }

        /**
         * 'getSyncState' retrieves the current synchronization state of the order book.
         */
        SyncState getSyncState() const noexcept { return state_; }

        /**
         * 'setSyncState' sets the current synchronization state of the order book.
         */
        void setSyncState(SyncState state) noexcept
        {
            state_ = state;
            synced_ = (state == SyncState::Synced);
        }

        /**
         * 'getAppliedSeqID' retrieves the last sequence ID that has been successfully applied to the order book.
         * 'setAppliedSeqID' sets the last sequence ID that has been successfully applied to the order book.
         */
        [[nodiscard]] std::uint64_t getAppliedSeqID() const noexcept { return last_seq_; }
        void setAppliedSeqID(std::uint64_t seq) { last_seq_ = seq; }

        /**
         * Buffer of incremental updates received while not synced.
         * 'getBuffer' retrieves the buffer of incremental updates.
         * 'pushToBuffer' adds an incremental update to the buffer.
         */
        void pushToBuffer(const GenericIncrementalFormat &update) { buffer_.push_back(update); }
        void popFromBuffer() { buffer_.pop_front(); }
        std::size_t bufferSize() const { return buffer_.size(); }

        /**
         * 'processBuffer' attempts to process buffered incremental updates after a snapshot has been applied.
         */
        [[nodiscard]] bool processBuffer();

    private:
        OrderBook book_;
        bool synced_{false};
        SyncState state_;
        std::deque<GenericIncrementalFormat> buffer_{};
        std::uint64_t last_seq_{0};
    };

}