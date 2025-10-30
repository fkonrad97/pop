#pragma once

#include "feed_handler.hpp"             // your IVenueFeedHandler, Status, FeedHandlerConfig
#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>

namespace md {
    class VenueFactory {
    public:
        /**
         * Create and initialize a venue-specific feed handler based on cfg.venue_name.
         * Returns nullptr if the venue is unknown or init() fails.
         *
         * Usage:
         *   auto fh = VenueFactory::create(ioc, cfg);
         *   if (!fh) { -handle error-  }
        *   fh->start();
        */
        static std::unique_ptr<IVenueFeedHandler>
        create(boost::asio::io_context &ioc, const FeedHandlerConfig &cfg);
    };
} // namespace md
