#pragma once

#include "abstract/feed_handler.hpp"
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <iostream>
#include <algorithm>    // std::transform / ranges
#include <boost/algorithm/string.hpp>
#include <cctype>       // std::tolower, std::toupper

namespace md {
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_okx_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_bybit_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_bitget_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_kucoin_feed_handler(boost::asio::io_context &ioc);

    namespace venue {
        struct Endpoint {
            std::string host; // e.g. "stream.binance.com"
            std::string port; // e.g. "9443"
            std::string path; // base path, e.g. "/ws/v5/public", may be ""
        };

        inline Endpoint default_rest_endpoint(VenueId v, MarketKind market_kind, AccessKind access_kind) {
            switch (v) {
                case VenueId::BINANCE: {
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    /// REST: https://api.binance.com/api/v3/
                                    return {"api.binance.com", "443", "/api/v3"};
                                }
                                default: throw std::invalid_argument("Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("Invalid access kind");
                    }
                }
                case VenueId::OKX: {
                    // OKX uses https://www.okx.com
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    return {"www.okx.com", "443", "/api/v5"};
                                }
                                default: throw std::invalid_argument("OKX: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("OKX: Invalid access kind");
                    }
                }
                case VenueId::BYBIT: {
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    return {"api.bybit.com", "443", "/v5"};
                                }
                                default: throw std::invalid_argument("BYBIT: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("BYBIT: Invalid access kind");
                    }
                }
                case VenueId::BITGET: {
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    return {"api.bitget.com", "443", "/api/spot/v1"};
                                }
                                default: throw std::invalid_argument("BITGET: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("BYBIT: Invalid access kind");
                    }
                }
                case VenueId::KUCOIN: {
                    // Kucoin spot REST: https://api.kucoin.com/api/v1/...
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    return {"api.kucoin.com", "443", "/api/v1/bullet-public"};
                                }
                                default: throw std::invalid_argument("KUCOIN: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("KUCOIN: Invalid access kind");
                    }
                }
                default:
                    throw std::invalid_argument("default_rest_endpoint: unknown VenueId");
            }
        }

        inline Endpoint default_ws_endpoint(VenueId v, MarketKind market_kind, AccessKind access_kind) {
            switch (v) {
                case VenueId::BINANCE: {
                    // For spot public we mostly embed everything into target,
                    // so path can stay empty.
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    return {"stream.binance.com", "9443", ""};
                                }
                                default: throw std::invalid_argument("[WS] BINANCE: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("[WS] BINANCE: Invalid access kind");
                    }
                }
                case VenueId::OKX: {
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    // Same public WS gateway; channel decides spot vs futures.
                                    return {"ws.okx.com", "8443", "/ws/v5/public"};
                                }
                                default: throw std::invalid_argument("[WS] OKX: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("[WS] OKX: Invalid access kind");
                    }
                }
                case VenueId::BYBIT: {
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    // official: wss://stream.bybit.com/v5/public/spot
                                    return {"stream.bybit.com", "443", "/v5/public/spot"};
                                }
                                default: throw std::invalid_argument("[WS] BYBIT: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("[WS] BYBIT: Invalid access kind");
                    }
                }
                case VenueId::BITGET: {
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    // e.g. wss://ws.bitget.com/spot/v1/stream
                                    return {"ws.bitget.com", "443", "/spot/v1/stream"};
                                }
                                default: throw std::invalid_argument("[WS] BITGET: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("[WS] BITGET: Invalid access kind");
                    }
                }
                case VenueId::KUCOIN: {
                    // For Kucoin, real endpoint comes from bullet-public,
                    // so the "default" here is just a placeholder.
                    switch (market_kind) {
                        case MarketKind::SPOT: {
                            switch (access_kind) {
                                case AccessKind::PUBLIC: {
                                    return {"ws-api-spot.kucoin.com", "443", "/"};
                                }
                                default: throw std::invalid_argument("[WS] KUCOIN: Invalid access kind");
                            }
                        }
                        default: throw std::invalid_argument("[WS] KUCOIN: Invalid access kind");
                    }
                }
                default:
                    throw std::invalid_argument("default_ws_endpoint: unknown VenueId");
            }
        }

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
                case StreamKind::DEPTH:
                    return handler.depthChannelResolver();
                default:
                    throw std::invalid_argument("resolve_stream_channel: StreamKind::UNKNOWN not allowed");
            }
        }
    } // namespace venue
} // namespace md
