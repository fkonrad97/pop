#include "brain/UnifiedBook.hpp"
#include "brain/JsonParsers.hpp"

#include <iostream>

namespace brain {

// ---------------------------------------------------------------------------
// VenueBook

VenueBook::VenueBook(std::string name, std::size_t depth)
    : venue_name(std::move(name)),
      controller(std::make_unique<md::OrderBookController>(depth)) {
    // Brain joins mid-stream: allow sequence gaps on all controllers.
    // No checksum function: PoP has already validated.
    controller->setAllowSequenceGap(true);
}

bool VenueBook::synced() const noexcept {
    return controller->isSynced();
}

const md::OrderBook &VenueBook::book() const noexcept {
    return controller->book();
}

// ---------------------------------------------------------------------------
// UnifiedBook

UnifiedBook::UnifiedBook(std::size_t depth) : depth_(depth) {}

VenueBook *UnifiedBook::find_or_create_(const std::string &venue) {
    for (auto &vb : books_) {
        if (vb.venue_name == venue) return &vb;
    }
    books_.emplace_back(venue, depth_);
    std::cerr << "[UnifiedBook] registered new venue: " << venue << "\n";
    return &books_.back();
}

std::size_t UnifiedBook::synced_count() const noexcept {
    std::size_t n = 0;
    for (const auto &vb : books_)
        if (vb.synced()) ++n;
    return n;
}

std::string UnifiedBook::on_event(const nlohmann::json &j) {
    EventHeader hdr;
    try {
        hdr = parse_header(j);
    } catch (...) {
        return {};
    }

    if (hdr.venue.empty() || hdr.event_type.empty()) return {};

    VenueBook *vb = find_or_create_(hdr.venue);

    try {
        if (hdr.event_type == "snapshot") {
            auto snap = parse_snapshot(j);
            vb->ts_book_ns = snap.ts_recv_ns;
            const auto action = vb->controller->onSnapshot(
                snap, md::OrderBookController::BaselineKind::WsAuthoritative);
            if (action == md::OrderBookController::Action::NeedResync)
                std::cerr << "[UnifiedBook] NeedResync after snapshot venue=" << hdr.venue << "\n";

        } else if (hdr.event_type == "incremental") {
            auto inc = parse_incremental(j);
            vb->ts_book_ns = inc.ts_recv_ns;
            const auto action = vb->controller->onIncrement(inc);
            if (action == md::OrderBookController::Action::NeedResync)
                std::cerr << "[UnifiedBook] NeedResync after incremental venue=" << hdr.venue
                          << " — awaiting next book_state\n";

        } else if (hdr.event_type == "book_state") {
            vb->ts_book_ns = extract_ts_book_ns(j);
            auto snap = parse_book_state_as_snapshot(j);
            const auto action = vb->controller->onSnapshot(
                snap, md::OrderBookController::BaselineKind::WsAuthoritative);
            if (action == md::OrderBookController::Action::NeedResync)
                std::cerr << "[UnifiedBook] NeedResync after book_state venue=" << hdr.venue << "\n";

        } else {
            return {};
        }
    } catch (const std::exception &e) {
        std::cerr << "[UnifiedBook] parse error venue=" << hdr.venue
                  << " type=" << hdr.event_type << " err=" << e.what() << "\n";
        return {};
    } catch (...) {
        std::cerr << "[UnifiedBook] unknown parse error venue=" << hdr.venue << "\n";
        return {};
    }

    return hdr.venue;
}

} // namespace brain
