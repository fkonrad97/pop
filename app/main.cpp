#include <boost/asio/io_context.hpp>
#include <boost/asio/signal_set.hpp>
#include <algorithm>
#include <iostream>
#include <memory>
#include <nlohmann/json.hpp>
#include <boost/asio.hpp>
#include <deque>
#include <sstream>

#include "feed_handler.hpp"
#include "rest_client.hpp"
#include "venue_factory.hpp"
#include "ws_client.hpp"

void test_ws_client(boost::asio::io_context &, const std::shared_ptr<md::WsClient> &);

void test_rest_client(boost::asio::io_context &, const std::shared_ptr<md::WsClient> &,
                      const std::shared_ptr<md::RestClient> &);

int test_binance_feed_handler(boost::asio::io_context &);

int main() {
    boost::asio::io_context ioc;
    const auto rest = std::make_shared<md::RestClient>(ioc);
    const auto ws = std::make_shared<md::WsClient>(ioc);

    test_rest_client(ioc, ws, rest);
    // test_ws_client(ioc, ws);
    // test_binance_feed_handler(ioc);

    return 0;
}

void test_ws_client(boost::asio::io_context &ioc, const std::shared_ptr<md::WsClient> &ws) {
    /**
     * WS client usage example:
     */
    // 2) Attach callbacks
    ws->set_on_message([](const std::string &msg) {
        // Print the first ~300 chars to keep logs readable
        std::cout << "[WS] " << (msg.size() > 300 ? msg.substr(0, 300) + "..." : msg) << "\n";
    });
    ws->set_on_close([] {
        std::cout << "[WS] closed\n";
    });

    // 3) Binance WS endpoint
    //    host:   stream.binance.com
    //    port:   9443
    //    target: /ws/<symbol>@depth OR @depth@100ms, etc.
    std::string symbol = "BTCUSDT";
    std::transform(symbol.begin(), symbol.end(), symbol.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    const std::string host = "stream.binance.com";
    const std::string port = "9443";
    const std::string target = "/ws/" + symbol + "@depth3"; // or "@depth@100ms", "@aggTrade", etc.

    // 4) Start the async connect chain
    ws->connect(host, port, target);

    // 5) Handle Ctrl+C for graceful shutdown
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([ws](const boost::system::error_code &, int) {
        std::cout << "\n[signal] shutting down...\n";
        ws->close();
    });

    // 6) Run event loop
    std::cout << "[main] running io_context...\n";
    ioc.run();
    std::cout << "[main] io_context stopped.\n";
}

// This one simulates with event based updates (calling @depth withouth giving level)
void test_rest_client(boost::asio::io_context &ioc,
                      const std::shared_ptr<md::WsClient> &ws,
                      const std::shared_ptr<md::RestClient> &rest_init) {
    using json = nlohmann::json;

    const std::string symbol      = "BTCUSDT";
    const std::string rest_host   = "api.binance.com";
    const std::string rest_port   = "443";
    const std::string rest_target = "/api/v3/depth?symbol=" + symbol + "&limit=1000";

    std::deque<json> ws_buffer;
    uint64_t lastUpdateId = 0;
    bool snapshot_ready   = false;
    bool rest_busy        = false;
    std::chrono::steady_clock::time_point last_resync_time;

    //-------------------------------------------------------
    // Declare resync function first
    //-------------------------------------------------------
    std::function<void()> resync;

    //-------------------------------------------------------
    // WebSocket handler (declared first to allow resync calls)
    //-------------------------------------------------------
    ws->set_on_message([&](const std::string &m) {
        try {
            json msg;
            std::stringstream(m) >> msg;
            if (!msg.contains("U") || !msg.contains("u")) return;

            uint64_t U = msg["U"].get<uint64_t>();
            uint64_t u = msg["u"].get<uint64_t>();

            ws_buffer.push_back(msg);

            if (snapshot_ready) {
                while (!ws_buffer.empty()) {
                    auto msg2 = ws_buffer.front();
                    ws_buffer.pop_front();

                    uint64_t U2 = msg2["U"].get<uint64_t>();
                    uint64_t u2 = msg2["u"].get<uint64_t>();

                    if (U2 <= lastUpdateId + 1 && u2 >= lastUpdateId) {
                        lastUpdateId = u2;
                        std::cout << "[WS] Applied update, last=" << lastUpdateId << "\n";
                    } else if (U2 > lastUpdateId + 1) {
                        std::cerr << "[WS] GAP detected! U=" << U2
                                  << " last=" << lastUpdateId << " → resyncing...\n";
                        if (resync) resync();
                        return;
                    }
                }
            }
        } catch (const std::exception &e) {
            std::cerr << "[WS] JSON error: " << e.what() << "\n";
        }
    });

    ws->set_on_close([] { std::cout << "[WS] closed\n"; });

    //-------------------------------------------------------
    // Define the resync() function after WS handler
    //-------------------------------------------------------
    resync = [&]() {
        auto now = std::chrono::steady_clock::now();

        if (rest_busy) {
            std::cout << "[RESYNC] skipped (busy)\n";
            return;
        }
        if (now - last_resync_time < std::chrono::seconds(2)) {
            std::cout << "[RESYNC] skipped (debounce)\n";
            return;
        }

        rest_busy = true;
        last_resync_time = now;
        snapshot_ready = false;

        std::cout << "[RESYNC] Fetching new snapshot...\n";

        // create a fresh RestClient every time to avoid SSL reuse errors
        auto rest = std::make_shared<md::RestClient>(ioc);
        rest->async_get(rest_host, rest_target, rest_port,
            [&](const boost::system::error_code &ec, const std::string &body) {
                rest_busy = false;

                if (ec) {
                    std::cerr << "[REST] error: " << ec.message() << "\n";
                    return;
                }

                json snap;
                std::stringstream(body) >> snap;
                lastUpdateId = snap["lastUpdateId"].get<uint64_t>();
                snapshot_ready = true;

                std::cout << "[REST] Snapshot loaded. lastUpdateId=" << lastUpdateId << "\n";
                if (snap.contains("bids") && snap.contains("asks"))
                    std::cout << "[REST] Top bid: " << snap["bids"][0]
                              << " | Top ask: " << snap["asks"][0] << "\n";

                // Remove outdated WS messages
                while (!ws_buffer.empty()) {
                    auto &msg = ws_buffer.front();
                    uint64_t u = msg["u"].get<uint64_t>();
                    if (u <= lastUpdateId)
                        ws_buffer.pop_front();
                    else
                        break;
                }

                // Apply first valid WS message
                while (!ws_buffer.empty()) {
                    auto msg = ws_buffer.front();
                    ws_buffer.pop_front();

                    uint64_t U = msg["U"].get<uint64_t>();
                    uint64_t u = msg["u"].get<uint64_t>();

                    if (U <= lastUpdateId + 1 && u >= lastUpdateId) {
                        lastUpdateId = u;
                        std::cout << "[SYNC] First update applied. lastUpdateId=" << lastUpdateId << "\n";
                    }
                }

                std::cout << "[SYNC] Snapshot + WS now aligned ✅\n";
            });
    };

    //-------------------------------------------------------
    // Connect WS first
    //-------------------------------------------------------
    std::string lower = symbol;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    ws->connect("stream.binance.com", "9443", "/ws/" + lower + "@depth@100ms");

    //-------------------------------------------------------
    // Initial resync call
    //-------------------------------------------------------
    resync();

    //-------------------------------------------------------
    // Run I/O context loop
    //-------------------------------------------------------
    ioc.run();
}

int test_binance_feed_handler(boost::asio::io_context &ioc) {
    md::FeedHandlerConfig cfg;
    cfg.venue_name = "BINANCE";
    cfg.symbol = "btcusdt";
    cfg.host_name = "stream.binance.com";
    cfg.port = "9443";
    cfg.target = "depth@100ms";

    auto fh = md::VenueFactory::create(ioc, cfg);
    if (!fh) return 1;

    if (fh->start() != md::Status::OK) return 1;

    ioc.run();
    return 0;
}
