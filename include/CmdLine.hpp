#pragma once

#include "abstract/FeedHandler.hpp"

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
    std::optional<std::string> ws_host; // override or std::nullopt
    std::optional<std::string> ws_port; // override or std::nullopt
    std::optional<std::string> ws_path; // override or std::nullopt
    std::optional<std::string> rest_host; // override or std::nullopt
    std::optional<std::string> rest_port; // override or std::nullopt
    std::optional<std::string> rest_path; // override or std::nullopt

    bool show_help{false};
};

inline md::StreamKind parse_stream_kind(const std::string &s_raw, const int depthLevel) {
    std::string s = s_raw;
    std::ranges::transform(s, s.begin(), ::tolower);

    if (s == "incremental") return md::StreamKind::INCREMENTAL;
    if (s == "depth") { 
        switch (depthLevel)
        {
        case 5:
            return md::StreamKind::DEPTH5;
        default:
            throw std::invalid_argument("Invalid depth level for depth stream kind");
        }
    }
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
            ("channel,c", po::value<std::string>()->default_value("depth"),
             "Stream type: incremental, depth")
            ("depthLevel,dl", po::value<int>(),
             "Orderbook depth; required/used if channel is depth")
            ("ws_host", po::value<std::string>(),
             "Optional WebSocket host override")
            ("ws_port", po::value<std::string>(),
             "Optional WebSocket port override")
            ("ws_path", po::value<std::string>(),
             "Optional WebSocket path override")
            ("rest_host", po::value<std::string>(),
             "Optional REST host override")
            ("rest_port", po::value<std::string>(),
             "Optional REST port override")
            ("rest_path", po::value<std::string>(),
             "Optional REST path override");

    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);

        if (vm.count("help")) {
            std::cout << "Usage: " << argv[0]
                    << " --venue VENUE --base BTC --quote USDT "
                    "[--channel incremental|depth] "
                    "[--depthLevel N] [--host HOST] [--port PORT] [--path PATH]\n\n";
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
    if (vm.contains("depthLevel")) out.depthLevel = vm["depthLevel"].as<int>();
    if (vm.contains("ws_host")) out.ws_host = vm["ws_host"].as<std::string>();
    if (vm.contains("ws_port")) out.ws_port = vm["ws_port"].as<std::string>();
    if (vm.contains("ws_path")) out.ws_path = vm["ws_path"].as<std::string>();
    if (vm.contains("rest_host")) out.rest_host = vm["rest_host"].as<std::string>();
    if (vm.contains("rest_port")) out.rest_port = vm["rest_port"].as<std::string>();
    if (vm.contains("rest_path")) out.rest_path = vm["rest_path"].as<std::string>();

    return true;
}
