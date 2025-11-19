#pragma once

#include "feed_handler.hpp"             // IVenueFeedHandler, Status, FeedHandlerConfig
#include <boost/asio/io_context.hpp>

#include <memory>
#include <string>
#include <iostream>
#include <algorithm>    // std::transform / ranges
#include <boost/algorithm/string.hpp>
#include <cctype>       // std::tolower, std::toupper

namespace md {
    // These are defined in your venue-specific headers (binance_feed_handler.hpp, okx_feed_handler.hpp)
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_okx_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_bybit_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_bitget_feed_handler(boost::asio::io_context &ioc);

    std::unique_ptr<IVenueFeedHandler> make_kucoin_feed_handler(boost::asio::io_context &ioc);

    namespace venue {
        enum class VenueId { BINANCE, OKX, BYBIT, BITGET, KUCOIN, UNKNOWN };

        inline VenueId to_venue_id(const std::string &name) {
            std::string v = name;
            boost::algorithm::to_lower(v);

            if (v == "binance") return VenueId::BINANCE;
            if (v == "okx")     return VenueId::OKX;
            if (v == "bybit")   return VenueId::BYBIT;
            if (v == "bitget")  return VenueId::BITGET;
            if (v == "kucoin")  return VenueId::KUCOIN;
            return VenueId::UNKNOWN;
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
            std::string v = cfg.venue_name;
            boost::algorithm::to_lower(v);
            std::unique_ptr<IVenueFeedHandler> handler;

            if (v == "binance") handler = make_binance_feed_handler(ioc);
            if (v == "okx")     handler = make_okx_feed_handler(ioc);
            if (v == "bybit")   handler = make_bybit_feed_handler(ioc);
            if (v == "bitget")  handler = make_bitget_feed_handler(ioc);
            if (v == "kucoin")  handler = make_kucoin_feed_handler(ioc);

            if (!handler) {
                return nullptr; // unknown venue
            }

            if (handler->init(cfg) != Status::OK) {
                return nullptr; // invalid config or precondition failed
            }

            return handler;
        }

        // --- 2) Symbol mapping ---

        /// Normalize a generic symbol - The pattern should be :: $symbol1-$symbol2 (e.g.: BTC-USDT)
        inline std::string map_ws_symbol(VenueId v, const std::string &sym) {
            std::vector<std::string> strs;
            boost::split(strs, sym, boost::is_any_of("-"));
            std::string concatSymbol = strs.at(0) + strs.at(1);

            switch (v) {
                case VenueId::BINANCE:
                    return boost::algorithm::to_lower_copy(concatSymbol);
                case VenueId::OKX:
                    return boost::algorithm::to_upper_copy(sym);
                case VenueId::BYBIT:
                    return boost::algorithm::to_upper_copy(concatSymbol);
                case VenueId::BITGET:
                    return boost::algorithm::to_upper_copy(concatSymbol);
                case VenueId::KUCOIN:
                    // KuCoin topics use "BTC-USDT"
                    return boost::algorithm::to_upper_copy(sym);
                default:
                    throw std::invalid_argument("Invalid Venue_ID/Symbol");
            }
        }

        /// --- 3) Logical depth target → venue-specific mapping ---

        /**
         * @brief Map a logical depth spec (e.g. "depth", "depth5@100ms")
         *        to a Binance WS suffix, WITHOUT symbol or "/ws/".
         *
         * Input examples:
         *   ""              -> "@depth"          (default)
         *   "depth"         -> "@depth"
         *   "depth5"        -> "@depth5"
         *   "depth5@100ms"  -> "@depth5@100ms"
         */
        inline std::string logical_to_binance_depth_suffix(const std::string &logical) {
            if (logical.empty() || logical == "depth") {
                return "@depth"; // Binance default depth stream
            }
            if (!logical.empty() && logical.front() == '@') {
                return logical; // already a suffix
            }
            return "@" + logical;
        }

        /**
         * @brief Map a logical depth spec to an OKX channel name.
         *
         * Input examples:
         *   "" / "depth"          -> "books5"
         *   "depth-tbt"           -> "books-l2-tbt"
         *   "books" / "books5" / "books-l2-tbt" -> used as-is.
         */
        inline std::string logical_to_okx_depth_channel(const std::string &logical) {
            if (logical.empty() || logical == "depth") {
                return "books5";
            }
            return logical;
        }

        /**
         * @brief Map a logical depth spec to an Bitget channel name.
         *
         * Input examples:
         *   "" / "depth"          -> "books1"
         *   "depth5"              -> "books5"
         *   ...
         */
        inline std::string logical_to_bitget_depth_channel(const std::string &logical) {
            std::string l = logical;
            boost::algorithm::to_lower(l);

            // Default: use 5 levels depth
            if (l.empty() || l == "depth" || l == "depth5") {
                return "books5";
            }
            if (l == "books")   return "books";
            if (l == "books1")  return "books1";
            if (l == "books5")  return "books5";
            if (l == "books15") return "books15";

            // Fallback: let caller pass a raw channel name
            return logical;
        }

        inline std::string logical_to_bybit_orderbook_prefix(const std::string &logical) {
            // Our "logical" config:
            //   "" / "depth"    -> orderbook.1
            //   "depth5"        -> does not exist in bybit
            //   "depth50"       -> orderbook.50
            //   "orderbook.5"   -> used as-is prefix (we'll append .SYMBOL outside)
            if (logical.empty() || logical == "depth") {
                return "orderbook.1";
            }
            if (logical.rfind("orderbook.", 0) == 0) {
                return logical; // already an orderbook.* prefix
            }
            if (logical == "depth5") {
                return "orderbook.5";
            }
            if (logical == "depth50") {
                return "orderbook.50";
            }
            // fallback: treat whatever is given as a prefix and let it own it
            return logical;
        }

        inline std::string logical_to_kucoin_depth_topic(const std::string &logical,
                                                 const std::string &symbol_upper_with_dash) {
            // Default: 5-level spot orderbook
            // "" / "depth" / "depth5" → "/spotMarket/level2Depth5:BTC-USDT"
            std::string l = logical;
            boost::algorithm::to_lower(l);

            if (l.empty() || l == "depth" || l == "depth5") {
                return "/spotMarket/level2Depth5:" + symbol_upper_with_dash;
            }

            if (l == "depth50") {
                return "/spotMarket/level2Depth50:" + symbol_upper_with_dash;
            }

            // if caller directly provides a KuCoin topic, just return it
            if (!l.empty() && l[0] == '/') {
                return logical;
            }

            // Fallback: treat logical as a suffix under /spotMarket/
            return "/spotMarket/" + logical + ":" + symbol_upper_with_dash;
        }


        // --- 4) High-level helper for WS target / channel construction ---

        /**
         * @brief Build a venue-specific WS "target" for depth streams.
         *
         * For BINANCE: returns full WS path (e.g. "/ws/btcusdt@depth5@100ms").
         * For OKX:     returns CHANNEL name only (e.g. "books5"), because OKX path is fixed.
         */
        inline std::string make_depth_target(VenueId v,
                                             const std::string &symbol,
                                             const std::string &logical_target) {
            switch (v) {
                case VenueId::BINANCE: {
                    std::string ws_sym = map_ws_symbol(v, symbol);
                    std::string suffix = logical_to_binance_depth_suffix(logical_target);
                    return "/ws/" + ws_sym + suffix;
                }
                case VenueId::OKX: {
                    return logical_to_okx_depth_channel(logical_target);
                }
                case VenueId::BYBIT: {
                    // Here we return the *full channel string* "orderbook.5.BTCUSDT"
                    std::string sym = map_ws_symbol(v, symbol); // "BTCUSDT"
                    std::string prefix = logical_to_bybit_orderbook_prefix(logical_target);
                    return prefix + "." + sym;
                }
                case VenueId::BITGET: {
                    // Return channel *only* ("books5", "books15", ...)
                    return logical_to_bitget_depth_channel(logical_target);
                }
                case VenueId::KUCOIN: {
                    // Build full topic string, e.g. "/spotMarket/level2Depth5:BTC-USDT"
                    std::string kucoin_sym = map_ws_symbol(VenueId::KUCOIN, symbol);
                    return logical_to_kucoin_depth_topic(logical_target, kucoin_sym);
                }
                default:
                    return logical_target;
            }
        }
    } // namespace venue
} // namespace md
