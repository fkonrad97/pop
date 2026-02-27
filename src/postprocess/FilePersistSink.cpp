#include "postprocess/FilePersistSink.hpp"

#include <chrono>
#include <filesystem>

namespace md {
    FilePersistSink::FilePersistSink(std::string path, std::string venue, std::string symbol)
        : path_(std::move(path)),
          venue_(std::move(venue)),
          symbol_(std::move(symbol)) {
        try {
            const std::filesystem::path p(path_);
            if (p.has_parent_path()) {
                std::filesystem::create_directories(p.parent_path());
            }
            out_.open(path_, std::ios::out | std::ios::app);
        } catch (...) {
            // Keep sink disabled if path setup/open fails.
        }
    }

    std::int64_t FilePersistSink::now_ns_() noexcept {
        const auto now = std::chrono::system_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
    }

    nlohmann::json FilePersistSink::levels_to_json_(const std::vector<Level> &levels) {
        nlohmann::json arr = nlohmann::json::array();
        for (const auto &lvl: levels) {
            arr.push_back({
                {"price", lvl.price},
                {"quantity", lvl.quantity},
                {"priceTick", lvl.priceTick},
                {"quantityLot", lvl.quantityLot}
            });
        }
        return arr;
    }

    nlohmann::json FilePersistSink::levels_from_book_(const OrderBook &book, std::size_t top_n, Side side) {
        nlohmann::json arr = nlohmann::json::array();
        for (std::size_t i = 0; i < top_n; ++i) {
            const Level *lvl = (side == Side::BID) ? book.bid_ptr(i) : book.ask_ptr(i);
            if (!lvl) break;
            arr.push_back({
                {"price", lvl->price},
                {"quantity", lvl->quantity},
                {"priceTick", lvl->priceTick},
                {"quantityLot", lvl->quantityLot}
            });
        }
        return arr;
    }

    void FilePersistSink::write_line_(const nlohmann::json &j) noexcept {
        if (!out_.is_open()) return;
        try {
            out_ << j.dump() << '\n';
            out_.flush();
        } catch (...) {
            // If writes fail, keep the feed alive; persistence is best-effort for now.
        }
    }

    void FilePersistSink::write_snapshot(const GenericSnapshotFormat &snap, std::string_view source) noexcept {
        nlohmann::json j;
        const auto ts_persist_ns = now_ns_();
        j["schema_version"] = 1;
        j["event_type"] = "snapshot";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = snap.ts_recv_ns;
        j["ts_persist_ns"] = ts_persist_ns;
        j["seq_first"] = snap.lastUpdateId;
        j["seq_last"] = snap.lastUpdateId;
        j["checksum"] = snap.checksum;
        j["bids"] = levels_to_json_(snap.bids);
        j["asks"] = levels_to_json_(snap.asks);
        write_line_(j);
    }

    void FilePersistSink::write_incremental(const GenericIncrementalFormat &inc, std::string_view source) noexcept {
        nlohmann::json j;
        const auto ts_persist_ns = now_ns_();
        j["schema_version"] = 1;
        j["event_type"] = "incremental";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = inc.ts_recv_ns;
        j["ts_persist_ns"] = ts_persist_ns;
        j["seq_first"] = inc.first_seq;
        j["seq_last"] = inc.last_seq;
        j["prev_last"] = inc.prev_last;
        j["checksum"] = inc.checksum;
        j["bids"] = levels_to_json_(inc.bids);
        j["asks"] = levels_to_json_(inc.asks);
        write_line_(j);
    }

    void FilePersistSink::write_book_state(const OrderBook &book,
                                           std::uint64_t applied_seq,
                                           std::size_t top_n,
                                           std::string_view source,
                                           std::int64_t ts_book_ns) noexcept {
        nlohmann::json j;
        const auto ts_persist_ns = now_ns_();
        j["schema_version"] = 1;
        j["event_type"] = "book_state";
        j["source"] = source;
        j["venue"] = venue_;
        j["symbol"] = symbol_;
        j["persist_seq"] = ++persist_seq_;
        j["ts_recv_ns"] = 0;
        j["ts_book_ns"] = ts_book_ns;
        j["ts_persist_ns"] = ts_persist_ns;
        j["applied_seq"] = applied_seq;
        j["top_n"] = top_n;
        j["bids"] = levels_from_book_(book, top_n, Side::BID);
        j["asks"] = levels_from_book_(book, top_n, Side::ASK);
        write_line_(j);
    }
}
