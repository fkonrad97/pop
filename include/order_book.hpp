#pragma once
#include <map>
#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <unordered_map>
#include <utility>
#include <array>

namespace md {
    enum class Side { Bid, Ask };

    struct Level {
        double px{0.0};
        double qty{0.0};
    };

    struct TopOfBook {
        std::optional<Level> bestBid;
        std::optional<Level> bestAsk;
    };

    // ---------- Default container policy (STL maps) ----------
    template<class Price = double, class Qty = double>
    struct TreeMapPolicy {
        using BidMap = std::map<Price, Qty, std::greater<Price> >; // bids high→low
        using AskMap = std::map<Price, Qty, std::less<Price> >; // asks low→high
    };

    // ---------- OrderBook (policy-driven; STL-only) ----------
    template<
        class Price = double,
        class Qty = double,
        class Container = TreeMapPolicy<Price, Qty> >
    class OrderBook {
    public:
        using BidMap = typename Container::BidMap;
        using AskMap = typename Container::AskMap;

        explicit OrderBook(std::string venue, std::string symbol, std::size_t max_depth = 5)
            : venue_(std::move(venue)), symbol_(std::move(symbol)), max_depth_(max_depth) {
        }

        // ----- Snapshot & Delta (Binance-style) -----
        template<class Snapshot>
        bool applySnapshot(const Snapshot &snap) {
            clear_();
            last_update_id_ = getLastUpdateId_(snap);

            const auto bids = getBids_(snap);
            for (const auto &pq: bids)
                upsert_(Side::Bid, toDouble_(pq[0]), toDouble_(pq[1]));

            const auto asks = getAsks_(snap);
            for (const auto &pq: asks)
                upsert_(Side::Ask, toDouble_(pq[0]), toDouble_(pq[1]));

            trimDepth_();
            snapshot_ready_ = true;
            return true;
        }

        template<class Delta>
        bool applyDelta(const Delta &d) {
            if (!snapshot_ready_) return false;

            const uint64_t U = getU_(d);
            const uint64_t u = getu_(d);

            // Gap ahead (missed updates) → signal resync.
            if (U > last_update_id_ + 1) return false;

            // Too old (already applied or before our next expected) → ignored, not applied.
            if (u < last_update_id_ + 1) return false;

            // Apply in-range/update
            const auto dbids = getDeltaBids_(d);
            for (const auto &pq: dbids)
                upsert_(Side::Bid, toDouble_(pq[0]), toDouble_(pq[1]));

            const auto dasks = getDeltaAsks_(d);
            for (const auto &pq: dasks)
                upsert_(Side::Ask, toDouble_(pq[0]), toDouble_(pq[1]));

            trimDepth_();
            last_update_id_ = u;
            return true;
        }


        // ----- Queries -----
        TopOfBook top() const {
            TopOfBook tob;
            if (!bids_.empty()) tob.bestBid = Level{bids_.begin()->first, bids_.begin()->second};
            if (!asks_.empty()) tob.bestAsk = Level{asks_.begin()->first, asks_.begin()->second};
            return tob;
        }

        std::vector<Level> levels(Side s, std::size_t depth) const {
            std::vector<Level> out;
            out.reserve(depth);
            if (s == Side::Bid) {
                std::size_t i = 0;
                for (auto it = bids_.begin(); it != bids_.end() && i < depth; ++it, ++i)
                    out.push_back(Level{it->first, it->second});
            } else {
                std::size_t i = 0;
                for (auto it = asks_.begin(); it != asks_.end() && i < depth; ++it, ++i)
                    out.push_back(Level{it->first, it->second});
            }
            return out;
        }

        void setMaxDepth(std::size_t d) {
            max_depth_ = d;
            trimDepth_();
        }

        std::size_t maxDepth() const { return max_depth_; }

        const std::string &venue() const { return venue_; }
        const std::string &symbol() const { return symbol_; }
        uint64_t lastUpdateId() const { return last_update_id_; }
        bool snapshotReady() const { return snapshot_ready_; }

        void reset() {
            clear_();
            snapshot_ready_ = false;
            last_update_id_ = 0;
        }

    private:
        BidMap bids_;
        AskMap asks_;
        std::string venue_;
        std::string symbol_;
        std::size_t max_depth_{5};
        uint64_t last_update_id_{0};
        bool snapshot_ready_{false};

        void upsert_(Side s, double px, double qty) {
            if (qty == 0.0) {
                if (s == Side::Bid) bids_.erase(px);
                else asks_.erase(px);
                return;
            }
            if (s == Side::Bid) bids_[px] = qty;
            else asks_[px] = qty;
        }

        void trimDepth_() {
            trimSide_(bids_, max_depth_);
            trimSide_(asks_, max_depth_);
        }

        template<class MapT>
        static void trimSide_(MapT &m, std::size_t depth) {
            if (m.size() <= depth) return;
            auto it = m.begin();
            std::advance(it, depth);
            m.erase(it, m.end());
        }

        void clear_() {
            bids_.clear();
            asks_.clear();
        }

        // ---- Extractors (JSON-lib-agnostic; use get<>() by value) ----
        template<class Snapshot>
        static uint64_t getLastUpdateId_(const Snapshot &s) {
            return s.at("lastUpdateId").template get<uint64_t>();
        }

        template<class Snapshot>
        static std::vector<std::array<std::string, 2> > getBids_(const Snapshot &s) {
            return s.at("bids").template get<std::vector<std::array<std::string, 2> > >();
        }

        template<class Snapshot>
        static std::vector<std::array<std::string, 2> > getAsks_(const Snapshot &s) {
            return s.at("asks").template get<std::vector<std::array<std::string, 2> > >();
        }

        template<class Delta>
        static uint64_t getU_(const Delta &d) {
            return d.at("U").template get<uint64_t>();
        }

        template<class Delta>
        static uint64_t getu_(const Delta &d) {
            return d.at("u").template get<uint64_t>();
        }

        template<class Delta>
        static std::vector<std::array<std::string, 2> > getDeltaBids_(const Delta &d) {
            return d.at("b").template get<std::vector<std::array<std::string, 2> > >();
        }

        template<class Delta>
        static std::vector<std::array<std::string, 2> > getDeltaAsks_(const Delta &d) {
            return d.at("a").template get<std::vector<std::array<std::string, 2> > >();
        }

        static double toDouble_(const std::string &s) {
            return std::stod(s);
        }
    };

    // ---------- Multi-venue manager ----------
    struct VenueSymbolKey {
        std::string venue;
        std::string symbol;

        bool operator==(const VenueSymbolKey &o) const noexcept {
            return venue == o.venue && symbol == o.symbol;
        }
    };

    struct VenueSymbolKeyHash {
        std::size_t operator()(const VenueSymbolKey &k) const noexcept {
            std::hash<std::string> h;
            return (h(k.venue) * 1315423911u) ^ h(k.symbol);
        }
    };

    template<
        class Price = double,
        class Qty = double,
        class Container = TreeMapPolicy<Price, Qty> >
    class OrderBookManager {
    public:
        using Book = OrderBook<Price, Qty, Container>;

        Book &getOrCreate(const std::string &venue, const std::string &symbol, std::size_t depth = 5) {
            VenueSymbolKey key{venue, symbol};
            auto it = books_.find(key);
            if (it == books_.end()) {
                it = books_.emplace(key, Book{venue, symbol, depth}).first;
            } else {
                it->second.setMaxDepth(depth);
            }
            return it->second;
        }

        std::optional<TopOfBook> top(const std::string &venue, const std::string &symbol) const {
            VenueSymbolKey key{venue, symbol};
            auto it = books_.find(key);
            if (it == books_.end()) return std::nullopt;
            return it->second.top();
        }

    private:
        std::unordered_map<VenueSymbolKey, Book, VenueSymbolKeyHash> books_;
    };
} // namespace md
