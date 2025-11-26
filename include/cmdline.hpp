#pragma once

#include "abstract/feed_handler.hpp"

#include <boost/program_options.hpp>
#include <string>
#include <iostream>
#include <algorithm>

struct CmdOptions {
    std::string venue; // required
    std::string base; // --base BTC
    std::string quote; // --quote USDT
    std::string channel; // required-ish, with default "depth"
    std::optional<int> depthLevel; // only meaningful for depth channels
    std::string market; // "spot" | "futures"
    std::string scope; // "public" | "private"
    std::optional<std::string> host; // override or std::nullopt
    std::optional<std::string> port; // override or std::nullopt

    bool show_help{false};
};

inline md::StreamKind parse_stream_kind(const std::string &s_raw) {
    std::string s = s_raw;
    std::ranges::transform(s, s.begin(), ::tolower);

    if (s == "incremental") return md::StreamKind::INCREMENTAL;
    if (s == "depth") return md::StreamKind::DEPTH;
    return md::StreamKind::UNKNOWN;
}

inline md::VenueId parse_venue(const std::string &v_raw) {
    std::string v = v_raw;
    std::ranges::transform(v, v.begin(), ::tolower);

    if (v == "binance") {
        return md::VenueId::BINANCE;
    }
    if (v == "okx") {
        return md::VenueId::OKX;
    }
    if (v == "bybit") {
        return md::VenueId::BYBIT;
    }
    if (v == "bitget") {
        return md::VenueId::BITGET;
    }
    if (v == "kucoin") {
        return md::VenueId::KUCOIN;
    }
    return md::VenueId::UNKNOWN;
}

inline md::MarketKind parse_market_kind(const std::string &raw) {
    std::string s = raw;
    std::ranges::transform(s, s.begin(), ::tolower);
    if (s == "spot") return md::MarketKind::SPOT;
    return md::MarketKind::UNKNOWN;
}

inline md::AccessKind parse_access_kind(const std::string &raw) {
    std::string s = raw;
    std::ranges::transform(s, s.begin(), ::tolower);
    if (s == "public") return md::AccessKind::PUBLIC;
    return md::AccessKind::UNKNOWN;
}

inline bool parse_cmdline(int argc, char **argv, CmdOptions &out) {
    namespace po = boost::program_options;

    po::options_description desc("Options");
    desc.add_options()
            ("help,h", "Show this help message")
            ("venue,v", po::value<std::string>()->required(),
             "Venue name (binance, okx, bybit, bitget, kucoin)")
            ("base", po::value<std::string>()->required(),
             "Base asset, e.g. BTC")
            ("quote", po::value<std::string>()->required(),
             "Quote asset, e.g. USDT")
            ("channel,t", po::value<std::string>()->default_value("depth"),
             "Stream type: incremental, depth")
            ("depthLevel", po::value<int>(),
             "Orderbook depth; required/used if channel is depth")
            ("market,m", po::value<std::string>()->default_value("spot"),
             "Market: spot, futures")
            ("scope", po::value<std::string>()->default_value("public"),
             "Scope: public, private")
            ("host", po::value<std::string>(),
             "Optional host override")
            ("port", po::value<std::string>(),
             "Optional port override");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << "Usage: " << argv[0]
                    << " --venue VENUE --base BTC --quote USDT "
                    "[--channel incremental|depth] "
                    "[--market spot|futures] "
                    "[--scope public|private] "
                    "[--depthLevel N] [--host HOST] [--port PORT]\n\n";
            std::cout << desc << "\n";
            out.show_help = true;
            return true;
        }

        // Enforce required options
        po::notify(vm);
    } catch (const po::error &e) {
        std::cerr << "Error parsing command line: " << e.what() << "\n\n";
        std::cerr << desc << "\n";
        return false;
    }

    out.venue = vm["venue"].as<std::string>();
    out.base = vm["base"].as<std::string>();
    out.quote = vm["quote"].as<std::string>();
    out.channel = vm["channel"].as<std::string>();
    out.market = vm["market"].as<std::string>();
    out.scope = vm["scope"].as<std::string>();

    if (vm.contains("host")) out.host = vm["host"].as<std::string>();
    if (vm.contains("port")) out.port = vm["port"].as<std::string>();
    if (vm.contains("depthLevel")) out.depthLevel = vm["depthLevel"].as<int>();

    return true;
}
