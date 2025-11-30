#include "client_connection_handlers/WsClient.hpp"
#include "client_connection_handlers/RestClient.hpp"
#include "abstract/FeedHandler.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>

#include "VenueUtils.hpp"
#include "orderbook/L2BookController.hpp"
#include "stream_parser/BinanceL2Parser.hpp"

using json = nlohmann::json;

namespace md {
    class BinanceFeedHandler final : public IVenueFeedHandler {
    public:
        explicit BinanceFeedHandler(boost::asio::io_context &ioc)
        : ioc_(ioc), ws_(std::make_shared<WsClient>(ioc)),
          rest_(std::make_shared<RestClient>(ioc)) {
        }

        /// 1. IVenueFeedHandler overrides ::
        Status init(const FeedHandlerConfig &cfg) override {
            if (running_.load())
                return Status::ERROR;

            cfg_ = cfg;

            const auto depth = static_cast<std::size_t>(cfg_.depthLevel);

            // Construct controller & parser *here*, when we actually know the depth
            book_controller_ = std::make_unique<L2BookController>(depth);
            parser_ = std::make_unique<BinanceL2Parser>(*book_controller_);


            // Wire WS -> parser
            ws_->set_on_raw_message(
                [this](const char *data, std::size_t len) {
                    if (!parser_) return; // safety

                    std::string_view msg{data, len};

                    // Choose which parser entrypoint to use by stream_kind
                    if (cfg_.stream_kind == StreamKind::DEPTH) {
                        parser_->on_l2_snapshot(msg);
                    } else {
                        parser_->on_l2_incremental(msg);
                    }

                    // Debug: print top-of-book after every message
                    if (const auto *book = order_book()) {
                        const auto &bb = book->best_bid();
                        const auto &ba = book->best_ask();

                        if (!bb.empty() && !ba.empty()) {
                            std::cout << "[BINANCE BBO] "
                                    << "bid=" << ticks_to_price(bb.price_ticks)
                                    << " qty=" << lots_to_qty(bb.qty_lots)
                                    << " | ask=" << ticks_to_price(ba.price_ticks)
                                    << " qty=" << lots_to_qty(ba.qty_lots)
                                    << '\n';
                        } else {
                            std::cout << "[BINANCE BBO] book empty or partially empty\n";
                        }
                    } else {
                        std::cout << "[BINANCE BBO] order_book() is null\n";
                    }
                });


            ws_->set_on_close([this]() {
                running_.store(false);
                // TODO: health/state notify if needed
            });
            return Status::OK;
        }

        const L2Book *order_book() const noexcept {
            return book_controller_ ? &book_controller_->book() : nullptr;
        }

        static double ticks_to_price(PriceTicks t) {
            return static_cast<double>(t) / 100.0; // inverse of parse_price_to_ticks
        }

        static double lots_to_qty(QtyLots q) {
            return static_cast<double>(q) / 1000.0; // inverse of parse_qty_to_lots
        }

        Status start() override {
            if (running_.exchange(true))
                return Status::ERROR; // already running

            const std::string host = cfg_.ws_host.empty()
                                         ? "stream.binance.com"
                                         : cfg_.ws_host;
            const std::string port = cfg_.ws_port.empty()
                                         ? "443"
                                         : cfg_.ws_port;

            const std::string target = venue::resolve_stream_channel(*this, cfg_);

            std::cout << "[BINANCE] Connecting to wss://"
                    << host << ":" << port << "/" << target << "\n";

            ws_->connect(host, port, target);
            return Status::OK;
        }

        Status stop() override {
            if (!running_.exchange(false))
                return Status::DISCONNECTED;
            ws_->close();
            return Status::CLOSED;
        }

        bool is_running() const override { return running_.load(); }

        /// 2. IChannelResolver overrides ::
        std::string incrementalChannelResolver() override {
            std::string prefix = "/ws/" + cfg_.symbol;
            return prefix + "@depth";
        }

        /**
         * @brief Map a logical depth spec (e.g. "depth", "depth5@100ms")
         *        to a Binance WS suffix, WITHOUT symbol or "/ws/".
         *
         * Input example:
         *   5        -> "@depth5@100ms"
         */
        std::string depthChannelResolver() override {
            std::string prefix = "/ws/" + cfg_.symbol;

            switch (cfg_.depthLevel) {
                case 5: {
                    return prefix + "@depth5";
                }
                default:
                    throw std::invalid_argument("Invalid depth level");
            }
        }

    private:
        using Clock = std::chrono::steady_clock;

        boost::asio::io_context &ioc_;
        std::shared_ptr<WsClient> ws_;
        std::shared_ptr<RestClient> rest_;
        FeedHandlerConfig cfg_{};
        std::atomic<bool> running_{false};
        std::unique_ptr<L2BookController> book_controller_;
        std::unique_ptr<BinanceL2Parser> parser_;
    };

    // ---- Maker symbol exported for VenueFactory
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context &ioc) {
        return std::make_unique<BinanceFeedHandler>(ioc);
    }
} // namespace md
