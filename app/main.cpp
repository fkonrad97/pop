#include "cmdline.hpp"              // CmdOptions, parse_cmdline, parse_stream_kind, parse_venue
#include "abstract/feed_handler.hpp"// FeedHandlerConfig, Status, StreamKind, VenueId
#include "venue_util.hpp"           // md::venue::createFeedHandler, md::to_string(VenueId)

#include <boost/asio/io_context.hpp>
#include <iostream>

int main(int argc, char** argv) {
    CmdOptions options;
    if (!parse_cmdline(argc, argv, options)) {
        // parse_cmdline already printed error/help on failure
        return 1;
    }

    if (options.show_help) {
        return 0;
    }

    // ---------------------------------------------------------------------
    // 1) Validate venue
    // ---------------------------------------------------------------------
    md::VenueId venue = parse_venue(options.venue);
    if (venue == md::VenueId::UNKNOWN) {
        std::cerr << "Error: unknown venue '" << options.venue
                  << "'. Expected one of: binance, okx, bybit, bitget, kucoin.\n";
        return 1;
    }

    // ---------------------------------------------------------------------
    // 2) Validate stream kind / channel
    // ---------------------------------------------------------------------
    md::StreamKind kind = parse_stream_kind(options.channel);
    if (kind == md::StreamKind::UNKNOWN) {
        std::cerr << "Error: unknown stream type '" << options.channel
                  << "'. Expected one of: incremental, depth.\n";
        return 1;
    }

    // ---------------------------------------------------------------------
    // 3) Validate market kind
    // ---------------------------------------------------------------------
    md::MarketKind market_kind = parse_market_kind(options.market);
    if (market_kind == md::MarketKind::UNKNOWN) {
        std::cerr << "Error: unknown market '" << options.market
                  << "'. Expected: spot, futures.\n";
        return 1;
    }

    // ---------------------------------------------------------------------
    // 4) Validate access kind
    // ---------------------------------------------------------------------
    md::AccessKind access_kind = parse_access_kind(options.scope);
    if (access_kind == md::AccessKind::UNKNOWN) {
        std::cerr << "Error: unknown scope '" << options.scope
                  << "'. Expected: public, private.\n";
        return 1;
    }

    // ---------------------------------------------------------------------
    // 5) Derive effective depthLevel
    // ---------------------------------------------------------------------
    int depth_level = 0;

    if (kind == md::StreamKind::DEPTH) {
        // For depth streams, depthLevel must be provided and > 0
        if (!options.depthLevel.has_value()) {
            std::cerr << "Error: --depthLevel is required when channel=depth\n";
            return 1;
        }

        depth_level = *options.depthLevel;
        if (depth_level <= 0) {
            std::cerr << "Error: --depthLevel must be > 0 (got "
                      << depth_level << ")\n";
            return 1;
        }
    } else {
        // For incremental streams depth level is not used; keep 0
        depth_level = 0;
    }

    // ---------------------------------------------------------------------
    // 4) Build FeedHandlerConfig from CLI options
    // ---------------------------------------------------------------------
    md::FeedHandlerConfig cfg;
    cfg.venue_name  = venue;                        // enum VenueId
    cfg.symbol      = md::venue::map_ws_symbol(venue, options.base, options.quote);               // e.g. "BTC-USDT"
    cfg.host_name   = options.host.value_or("");    // "" → venue default host
    cfg.port        = options.port.value_or("");    // "" → venue default port
    cfg.stream_kind = kind;
    cfg.depthLevel  = depth_level;
    cfg.market_kind  = market_kind;
    cfg.access_kind  = access_kind;

    // Optional debug log
    std::cout << "[POP] Starting feed\n"
              << "  venue      = " << md::to_string(cfg.venue_name) << "\n"
              << "  symbol     = " << cfg.symbol << "\n"
              << "  channel    = " << options.channel
              << " (StreamKind=" << md::to_string(cfg.stream_kind) << ")\n"
              << "  depthLevel = " << cfg.depthLevel << "\n"
              << "  host       = " << (cfg.host_name.empty() ? "<default>" : cfg.host_name) << "\n"
              << "  port       = " << (cfg.port.empty() ? "<default>" : cfg.port) << "\n";

    // ---------------------------------------------------------------------
    // 5) Event loop + feed handler
    // ---------------------------------------------------------------------
    boost::asio::io_context ioc;

    auto fh = md::venue::createFeedHandler(ioc, cfg);
    if (!fh) {
        std::cerr << "Failed to create feed handler for venue="
                  << md::to_string(cfg.venue_name) << "\n";
        return 1;
    }

    if (fh->init(cfg) != md::Status::OK) {
        std::cerr << "init() failed\n";
        return 1;
    }

    if (fh->start() != md::Status::OK) {
        std::cerr << "start() failed\n";
        return 1;
    }

    ioc.run();
    return 0;
}