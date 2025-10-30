#include "venue_factory.hpp"
#include <algorithm>   // std::transform
#include <cctype>      // std::tolower

namespace md {

    // ---- Maker functions exposed by venue-specific .cpp files (no headers needed) ----
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context& ioc);

    // helper: lowercase a string (ASCII ok for venue keys)
    static std::string to_lower(std::string s) {
        std::ranges::transform(s, s.begin(),
                               [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::unique_ptr<IVenueFeedHandler> VenueFactory::create(boost::asio::io_context& ioc, const FeedHandlerConfig& cfg) {
        const std::string v = to_lower(cfg.venue_name);

        std::unique_ptr<IVenueFeedHandler> handler;

        if (v == "binance") {
            handler = make_binance_feed_handler(ioc);
        }

        if (!handler) {
            return nullptr; // unknown venue
        }

        if (handler->init(cfg) != Status::OK) {
            return nullptr; // invalid config or precondition failed
        }

        return handler;
    }

} // namespace md
