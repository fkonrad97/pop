#include "client_connection_handlers/WsClient.hpp"
#include "client_connection_handlers/RestClient.hpp"
#include "abstract/FeedHandler.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <atomic>
#include "VenueUtils.hpp"
#include "orderbook/BinanceOrderBookController.hpp"
#include "stream_parser/BinanceStreamParser.hpp"
#include "orderbook/OrderBookUtils.hpp"

using json = nlohmann::json;

namespace md
{
    class BinanceFeedHandler final : public IVenueFeedHandler
    {
    public:
        explicit BinanceFeedHandler(boost::asio::io_context &ioc)
            : ioc_(ioc), ws_(std::make_shared<WsClient>(ioc))
        {
        }

        /// 1. IVenueFeedHandler overrides ::
        Status init(const FeedHandlerConfig &cfg) override
        {
            if (running_.load())
                return Status::ERROR;

            cfg_ = cfg;

            const auto depth = static_cast<std::size_t>(cfg_.depthLevel);

            this->setVenueAddresses();

            // Construct controller + parser here, when we know depth
            ctrl_ = std::make_unique<BinanceOrderBookController>(depth);
            parser_ = std::make_unique<BinanceStreamParser>();

            ws_->set_on_open([this]()
                             {
                                /// Upon WS open, we can start receiving messages
                                std::cout << "[BINANCE] WebSocket connection established.\n";

                                 /// Reset controller state/book/buffer
                                ctrl_->reset();

                                /// Trigger initial snapshot request
                                request_snapshot(); });

            // Wire WS -> parser -> controller
            ws_->set_on_raw_message(
                [this](const char *data, std::size_t len)
                {
                    if (!parser_ || !ctrl_)
                        return;

                    std::string_view msg{data, len};

                    auto upd_opt = parser_->parse_incremental(msg);
                    if (!upd_opt)
                    {
                        return; // not a depthUpdate
                    }

                    auto action = ctrl_->on_increment(*upd_opt);

                    std::cout << "[BINANCE] received depth update U="
                              << upd_opt->U << " u=" << upd_opt->u << " bids=" << upd_opt->bids.size() << " asks=" << upd_opt->asks.size() << "\n";

                    if (action == BinanceOrderBookController::Action::NeedResync)
                    {
                        // A gap was detected; get a new snapshot
                        request_snapshot();
                    }

                    if (ctrl_->isSynced())
                    {
                        const auto &book = ctrl_->book();
                        const auto &bb = book.best_bid();
                        const auto &ba = book.best_ask();

                        std::cout << "[BINANCE BOOK] "
                                  << "bid=" << bb.price_ticks
                                  << " qty=" << bb.qty_lots
                                  << " size=" << book.bids().size()
                                  << " | ask=" << ba.price_ticks
                                  << " qty=" << ba.qty_lots
                                  << " size=" << book.asks().size()
                                  << '\n';
                    }
                });

            ws_->set_on_close([this]()
                              {
                                  running_.store(false);
                                  // TODO: health/state notify if needed
                              });

            return Status::OK;
        }

        Status start() override
        {
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

        Status stop() override
        {
            if (!running_.exchange(false))
                return Status::DISCONNECTED;
            ws_->close();
            return Status::CLOSED;
        }

        bool is_running() const override { return running_.load(); }

        /// 2. IChannelResolver overrides ::
        std::string incrementalChannelResolver() override
        {
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
        std::string depthChannelResolver() override
        {
            std::string prefix = "/ws/" + cfg_.symbol;

            switch (cfg_.depthLevel)
            {
            case 5:
            {
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
        std::unique_ptr<BinanceOrderBookController> ctrl_;
        std::unique_ptr<BinanceStreamParser> parser_;

        /// Flags ::
        std::atomic<bool> running_{false};

        /// Functions:
        void setVenueAddresses()
        {
            /// ws host/port/path:
            if (cfg_.ws_host.empty())
                cfg_.ws_host = "stream.binance.com";
            if (cfg_.ws_port.empty())
                cfg_.ws_port = "443";
            cfg_.ws_path = venue::resolve_stream_channel(*this, cfg_);

            /// rest host/port/path:
            if (cfg_.rest_host.empty())
                cfg_.rest_host = "api.binance.com";
            if (cfg_.rest_port.empty())
                cfg_.rest_port = "443";
            if (cfg_.depthLevel == 0)
            { /// Full OB
                cfg_.rest_path = "/api/v3/depth?symbol=" + md::venue::map_rest_symbol(cfg_.venue_name, cfg_.base_ccy, cfg_.quote_ccy);
            }
            else
            {
                cfg_.rest_path = "/api/v3/depth?symbol=" + md::venue::map_rest_symbol(cfg_.venue_name, cfg_.base_ccy, cfg_.quote_ccy) +
                                 "&limit=" + std::to_string(cfg_.depthLevel);
            }
        }

        void request_snapshot()
        {
            if (!parser_ || !ctrl_)
                return;

            auto rest = std::make_shared<RestClient>(ioc_);

            std::cout << "[BINANCE][REST] requesting snapshot https://"
                      << cfg_.rest_host << ":" << cfg_.rest_port << cfg_.rest_path << "\n";

            rest->async_get(
                cfg_.rest_host, cfg_.rest_port, cfg_.rest_path,
                [this, rest](boost::system::error_code ec, const std::string &body)
                {
                    if (ec)
                    {
                        std::cerr << "[BINANCE][REST] snapshot error: "
                                  << ec.message() << "\n";
                        return;
                    }
                    if (!parser_ || !ctrl_)
                        return;

                    auto snap_opt = parser_->parse_snapshot(body);
                    if (!snap_opt)
                    {
                        std::cerr << "[BINANCE][REST] failed to parse snapshot. Body: "
                                  << body << "\n";
                        return;
                    }
                    ctrl_->on_snapshot(*snap_opt);
                });
        }
    };

    // ---- Maker symbol exported for VenueFactory
    std::unique_ptr<IVenueFeedHandler> make_binance_feed_handler(boost::asio::io_context &ioc)
    {
        return std::make_unique<BinanceFeedHandler>(ioc);
    }
} // namespace md
