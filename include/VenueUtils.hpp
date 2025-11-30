#pragma once

#include "abstract/FeedHandler.hpp"
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <boost/algorithm/string.hpp>

namespace md {
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_okx_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_bybit_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_bitget_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_kucoin_feed_handler(boost::asio::io_context &ioc);

    namespace venue {
        /**
         * @brief Create and initialize a venue-specific feed handler based on cfg.venue_name.
         *        Returns nullptr if the venue is unknown or init() fails.
         *
         * Usage:
         *   auto fh = md::venue::createFeedHandler(ioc, cfg);
         *   if (!fh) { /* handle error }
         *   fh->start()
         */
        inline std::unique_ptr<IVenueFeedHandler> createFeedHandler(boost::asio::io_context &ioc,
                                                                    const FeedHandlerConfig &cfg) {
            std::unique_ptr<IVenueFeedHandler> handler;

            switch (cfg.venue_name) {
                case VenueId::BINANCE: handler = make_binance_feed_handler(ioc);
                    break;
                case VenueId::OKX: handler = make_okx_feed_handler(ioc);
                    break;
                case VenueId::BYBIT: handler = make_bybit_feed_handler(ioc);
                    break;
                case VenueId::BITGET: handler = make_bitget_feed_handler(ioc);
                    break;
                case VenueId::KUCOIN: handler = make_kucoin_feed_handler(ioc);
                    break;
                default: handler = nullptr;
            }

            return handler;
        }

        // --- 2) Symbol mapping ---
        inline std::string map_ws_symbol(VenueId venue,
                                         const std::string &base,
                                         const std::string &quote) {
            // Normalize to UPPER once
            const std::string base_up = boost::algorithm::to_upper_copy(base);
            const std::string quote_up = boost::algorithm::to_upper_copy(quote);

            const std::string concat = base_up + quote_up; // "BTCUSDT"
            const std::string dashed = base_up + "-" + quote_up; // "BTC-USDT"

            switch (venue) {
                case VenueId::BINANCE:
                    // Binance WS paths expect lowercase "btcusdt"
                    return boost::algorithm::to_lower_copy(concat);

                case VenueId::OKX:
                    // OKX uses "BTC-USDT"
                    return dashed;

                case VenueId::BYBIT:
                    // Bybit uses "BTCUSDT" in topics
                    return concat;

                case VenueId::BITGET:
                    // Bitget also uses "BTCUSDT"
                    return concat;

                case VenueId::KUCOIN:
                    // KuCoin uses "BTC-USDT" in topics
                    return dashed;

                default:
                    throw std::invalid_argument("map_ws_symbol: unknown VenueId");
            }
        }

        // type must derive from IVenueFeedHandler
        template<typename T>
        concept VenueFeedHandler = std::derived_from<T, IVenueFeedHandler>;

        /**
         * @brief Generic stream-channel resolver.
         *
         * Dispatches based on cfg.stream_kind and calls the appropriate
         * venue-specific resolver on the handler.
         *
         * Requirements:
         *   - T must inherit from IVenueFeedHandler (thus from IChannelResolver).
         *   - T must implement:
         *       std::string incrementalChannelResolver();
         *       std::string depthChannelResolver();
         */
        template<VenueFeedHandler T>
        std::string resolve_stream_channel(T &handler, const FeedHandlerConfig &cfg) {
            switch (cfg.stream_kind) {
                case StreamKind::INCREMENTAL:
                    return handler.incrementalChannelResolver();
                case StreamKind::DEPTH5:
                    return handler.depthChannelResolver();
                default:
                    throw std::invalid_argument("resolve_stream_channel: StreamKind::UNKNOWN not allowed");
            }
        }
    } // namespace venue
} // namespace md
