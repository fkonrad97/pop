#include "md/GenericFeedHandler.hpp"
#include <chrono>
#include <iostream>

namespace md {
    GenericFeedHandler::GenericFeedHandler(boost::asio::io_context &ioc): ioc_(ioc),
                                                                          ws_(WsClient::create(ioc)),
                                                                          rest_(RestClient::create(ioc)),
                                                                          reconnect_timer_(ioc) {
        rest_->set_keep_alive(true); // strongly recommended for snapshots
        rest_->set_logger([](std::string_view s) {
            // plug into your logging system; std::cerr is fine for now
            std::cerr << s << "\n";
        });
        // should be decreased for prod
        rest_->set_timeout(std::chrono::milliseconds(8000));
        rest_->set_shutdown_timeout(std::chrono::milliseconds(2000));
    }

    std::string GenericFeedHandler::makeConnectId() const {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        return std::to_string(ms);
    }

    GenericFeedHandler::AnyAdapter GenericFeedHandler::makeAdapter(VenueId v) {
        switch (v) {
            case VenueId::BINANCE: return BinanceAdapter{};
            case VenueId::OKX: return OKXAdapter{};
            case VenueId::BITGET: return BitgetAdapter{};
            case VenueId::BYBIT: return BybitAdapter{};
            case VenueId::KUCOIN: return KucoinAdapter{};
            default: return BinanceAdapter{};
        }
    }

    Status GenericFeedHandler::init(const FeedHandlerConfig &cfg) {
        if (running_.load()) return Status::ERROR;
        if (cfg.depthLevel == 0) return Status::ERROR;

        cfg_ = cfg;

        adapter_ = makeAdapter(cfg.venue_name);
        rt_.caps = std::visit([&](auto const &a) { return a.caps(); }, adapter_);

        rt_.venue = cfg_.venue_name;
        rt_.depth = cfg_.depthLevel;

        /// Resolve endpoints + prebuild frames/targets once (Cold Path)
        rt_.ws = std::visit([&](auto const &a) { return a.wsEndpoint(cfg_); }, adapter_);
        rt_.rest = std::visit([&](auto const &a) { return a.restEndpoint(cfg_); }, adapter_);

        if (!cfg_.rest_host.empty()) rt_.rest.host = cfg_.rest_host;
        if (!cfg_.rest_port.empty()) rt_.rest.port = cfg_.rest_port;
        if (!cfg_.rest_path.empty()) rt_.restSnapshotTarget = cfg_.rest_path;

        rt_.wsSubscribeFrame = std::visit([&](auto const &a) { return a.wsSubscribeFrame(cfg_); }, adapter_);
        rt_.restSnapshotTarget = std::visit([&](auto const &a) { return a.restSnapshotTarget(cfg_); }, adapter_);

        controller_ = std::make_unique<OrderBookController>(rt_.depth);
        controller_->configureChecksum(rt_.caps.checksum_fn, rt_.caps.checksum_top_n);
        // KuCoin and a few other venues may emit non-contiguous sequence numbers
        // especially when using partial REST snapshots.  Allow controller to
        // tolerate gaps if requested by the adapter via VenueCaps.
        controller_->setAllowSequenceGap(rt_.caps.allow_seq_gap);
        if (rt_.caps.allow_seq_gap) {
            std::cerr << "[GenericFeedHandler] ALLOW_SEQ_GAP enabled for venue\n";
        }

        buffer_.clear();
        state_ = SyncState::DISCONNECTED;

        return Status::OK;
    }

    Status GenericFeedHandler::start() {
        if (running_.exchange(true)) {
            return Status::ERROR;
        }

        state_ = SyncState::CONNECTING;

        /// Wire WS callback once
        ws_->set_on_open([this] { onWSOpen(); });
        ws_->set_on_raw_message([this](const char *data, std::size_t len) { onWSMessage(data, len); });
        ws_->set_on_close([this] {
            if (!running_.load(std::memory_order_acquire)) return;
            onWSClose_();
        });

        connect_id_ = makeConnectId();

        if (rt_.caps.requires_ws_bootstrap) {
            state_ = SyncState::BOOTSTRAPPING;
            bootstrapWS();
            return Status::OK;
        }

        connectWS();
        return Status::OK;
    }

    Status GenericFeedHandler::stop() {
        running_.store(false, std::memory_order_release);

        if (rest_) rest_->cancel();
        if (ws_) ws_->close();

        state_ = SyncState::DISCONNECTED;
        buffer_.clear();

        if (controller_) controller_->resetBook();
        return Status::OK;
    }



    /**
     * - Generic WS connect uses resolved endpoint
     */
    void GenericFeedHandler::connectWS() {
        if (rt_.ws_ping_interval_ms > 0) {
            ws_->set_idle_ping(std::chrono::milliseconds(rt_.ws_ping_interval_ms));
        } else {
            ws_->set_idle_ping(std::chrono::milliseconds(0));
        }
        ws_->connect(rt_.ws.host, rt_.ws.port, rt_.ws.target);
    }

    /**
     * Subscribe to stream
     */
    void GenericFeedHandler::onWSOpen() {
        if (!running_.load()) return;

        if (!rt_.wsSubscribeFrame.empty()) {
            ws_->send_text(rt_.wsSubscribeFrame);
        }

        if (rt_.caps.sync_mode == SyncMode::RestAnchored) {
            state_ = SyncState::WAIT_REST_SNAPSHOT;
            std::cout << "[GenericFeedHandler] WS open (RestAnchored), requesting REST snapshot...\n";
            requestSnapshot();
        } else {
            state_ = SyncState::WAIT_WS_SNAPSHOT;
            std::cout << "[GenericFeedHandler] WS open (WsAuthoritative), waiting for WS snapshot...\n";
        }
    }

    /**
     * - Async GET snapshot
     */
    void GenericFeedHandler::requestSnapshot() {
        state_ = SyncState::WAIT_REST_SNAPSHOT;

        rest_->async_get(rt_.rest.host, rt_.rest.port, rt_.restSnapshotTarget,
                         [this](boost::system::error_code ec, std::string body) {
                             if (!running_.load(std::memory_order_acquire)) return;

                             if (ec) {
                                 // network / TLS / timeout errors
                                 restartSync();
                                 return;
                             }

                             const int status = rest_->last_http_status();

                             if (status == 429 || status == 418) {
                                 /// Rate-limited / temporary ban -> do NOT hammer.
                                 /// Simple fixed delay; replace with exponential backoff later.
                                 reconnect_timer_.expires_after(std::chrono::milliseconds(750));
                                 reconnect_timer_.async_wait([this](const boost::system::error_code &ec) {
                                     if (ec) return;
                                     if (!running_.load()) return;
                                     requestSnapshot();
                                 });
                                 return;
                             }

                             if (status < 200 || status >= 300) {
                                 // 4xx/5xx -> handle separately if you want
                                 restartSync();
                                 return;
                             }

                             onSnapshotResponse(body);
                         }
        );
    }


    void GenericFeedHandler::onSnapshotResponse(std::string_view body) {
        if (!running_.load()) return;

        GenericSnapshotFormat snap;
        const bool ok = std::visit([&](auto const &a) {
            return a.parseSnapshot(body, snap);
        }, adapter_);

        if (!ok) {
            restartSync();
            return;
        }

        const auto kind =
                (rt_.caps.sync_mode == SyncMode::RestAnchored)
                    ? OrderBookController::BaselineKind::RestAnchored
                    : OrderBookController::BaselineKind::WsAuthoritative;

        controller_->onSnapshot(snap, kind);

        /// Baseline loaded. We are not necessarily synced yet (RestAnchored must bridge).
        state_ = SyncState::WAIT_BRIDGE;

        /// Apply buffered incrementals after snapshot
        drainBufferedIncrementals();

        if (controller_->isSynced()) {
            std::cerr << "[GenericFeedHandler] bridged (post-snapshot drain) -> SYNCED\n";
            state_ = SyncState::SYNCED;
        } else {
            std::cerr << "[GenericFeedHandler] still WAIT_BRIDGE after drain\n";
        }
    }

    void GenericFeedHandler::drainBufferedIncrementals() {
        while (!buffer_.empty()) {
            GenericIncrementalFormat inc;

            const std::string &msg = buffer_.front();

            const bool ok = std::visit([&](auto const &a) {
                if (!a.isIncremental(msg)) return false;
                return a.parseIncremental(msg, inc);
            }, adapter_);

            buffer_.pop_front();

            if (!ok) continue;

            const auto action = controller_->onIncrement(inc);
            if (action == OrderBookController::Action::NeedResync) {
                restartSync();
                return;
            }
        }
    }

    void GenericFeedHandler::onWSMessage(const char *data, std::size_t len) {
        if (!running_.load() || len == 0) return;
        std::string_view msg{data, len};

        if (state_ == SyncState::WAIT_REST_SNAPSHOT) {
            // buffer incrementals
            const bool isInc = std::visit([&](auto const &a) { return a.isIncremental(msg); }, adapter_);
            if (isInc) {
                if (buffer_.size() < max_buffer_) buffer_.emplace_back(msg);
                else restartSync();
            }
            return;
        }

        if (state_ == SyncState::WAIT_WS_SNAPSHOT) {
            // first, try snapshot
            GenericSnapshotFormat snap;
            const bool isSnap = std::visit([&](auto const &a) {
                return a.isSnapshot(msg) && a.parseWsSnapshot(msg, snap);
            }, adapter_);

            if (isSnap) {
                controller_->onSnapshot(snap, OrderBookController::BaselineKind::WsAuthoritative);

                // baseline is WS snapshot; any buffered msgs were pre-baseline, drain them now
                state_ = SyncState::WAIT_BRIDGE;
                drainBufferedIncrementals();
                if (controller_->isSynced()) state_ = SyncState::SYNCED;
                return;
            }

            // otherwise buffer incrementals
            const bool isInc = std::visit([&](auto const &a) { return a.isIncremental(msg); }, adapter_);
            if (isInc) {
                if (buffer_.size() < max_buffer_) buffer_.emplace_back(msg);
                else restartSync();
            }
            return;
        }

        // WAIT_BRIDGE and SYNCED:
        // 1) For WS-authoritative venues, allow an "interrupting" WS snapshot at ANY time and re-baseline.
        if ((state_ == SyncState::WAIT_BRIDGE || state_ == SyncState::SYNCED) && rt_.caps.ws_sends_snapshot) {
            GenericSnapshotFormat snap;
            const bool isSnap = std::visit([&](auto const &a) {
                return a.isSnapshot(msg) && a.parseWsSnapshot(msg, snap);
            }, adapter_);

            if (isSnap) {
                // Hard re-baseline (venue may resend snapshot on internal resync)
                controller_->onSnapshot(snap, OrderBookController::BaselineKind::WsAuthoritative);

                // Any buffered incrementals are stale relative to this new baseline.
                buffer_.clear();

                // WS-authoritative snapshot implies we can treat it as baseline-loaded immediately.
                // Controller may set Synced directly; keep handler consistent.
                state_ = controller_->isSynced() ? SyncState::SYNCED : SyncState::WAIT_BRIDGE;
                return;
            }
        }

        // 2) Otherwise: parse + apply incrementals
        if (state_ == SyncState::WAIT_BRIDGE || state_ == SyncState::SYNCED) {
            // --- RestAnchored: during WAIT_BRIDGE we ONLY buffer+drain ---
            if (rt_.caps.sync_mode == SyncMode::RestAnchored && state_ == SyncState::WAIT_BRIDGE) {
                const bool isInc = std::visit([&](auto const &a) { return a.isIncremental(msg); }, adapter_);
                if (!isInc) return;

                if (buffer_.size() < max_buffer_) buffer_.emplace_back(msg);
                else {
                    restartSync();
                    return;
                }

                // Try to bridge using the same pipeline as post-snapshot drain
                drainBufferedIncrementals();
                if (controller_->isSynced()) {
                    std::cerr << "[GenericFeedHandler] bridged (ws buffered path) -> SYNCED\n";
                    state_ = SyncState::SYNCED;
                }
                return;
            }

            // --- Otherwise: steady-state apply (SYNCED, or WS-authoritative venues) ---
            GenericIncrementalFormat inc;
            const bool ok = std::visit([&](auto const &a) {
                if (!a.isIncremental(msg)) return false;
                return a.parseIncremental(msg, inc);
            }, adapter_);

            if (!ok) return;

            const auto action = controller_->onIncrement(inc);
            if (action == OrderBookController::Action::NeedResync) {
                restartSync();
                return;
            }

            if (state_ == SyncState::WAIT_BRIDGE && controller_->isSynced()) {
                std::cerr << "[GenericFeedHandler] bridged (ws path) -> SYNCED\n";
                state_ = SyncState::SYNCED;
            }
            return;
        }

        // Any other state: ignore
        return;
    }

    void GenericFeedHandler::restartSync() {
        if (!running_.load()) return;

        buffer_.clear();
        controller_->resetBook();

        // Reset state before reconnect
        state_ = SyncState::CONNECTING;

        // Correlate bootstrap if needed
        connect_id_ = makeConnectId();

        // Force-close current WS (immediate) and reconnect after a short backoff.
        closing_for_restart_ = true;
        ws_->cancel();

        schedule_ws_reconnect_(std::chrono::milliseconds(200));
    }


    void GenericFeedHandler::bootstrapWS() {
        if (!running_.load()) return;

        const std::string target = std::visit([&](auto const &a) {
            return a.wsBootstrapTarget(cfg_);
        }, adapter_);

        if (target.empty()) {
            // caps say we require bootstrap but adapter can't provide it -> hard fail
            std::cerr << "[GenericFeedHandler] ERROR: venue requires WS bootstrap but adapter did not provide target\n";
            restartSync();
            return;
        }

        const std::string body = std::visit([&](auto const &a) {
            return a.wsBootstrapBody(cfg_);
        }, adapter_);

        // POST bullet-public
        rest_->async_post(rt_.rest.host, rt_.rest.port, target, body,
                          [this](boost::system::error_code ec, const std::string &resp_body) {
                              if (ec) {
                                  restartSync();
                                  return;
                              }

                              WsBootstrapInfo info;
                              const bool ok = std::visit([&](auto const &a) {
                                  return a.parseWsBootstrap(resp_body, connect_id_, info);
                              }, adapter_);

                              if (!ok) {
                                  restartSync();
                                  return;
                              }

                              // overwrite resolved WS endpoint from bootstrap
                              rt_.ws = info.ws;
                              rt_.ws_ping_interval_ms = info.ping_interval_ms;
                              rt_.ws_ping_timeout_ms = info.ping_timeout_ms;

                              // now connect
                              connectWS();
                          }
        );
    }

    void GenericFeedHandler::onWSClose_() {
        // If we initiated the close as part of restartSync(), do NOT re-enter restartSync().
        if (closing_for_restart_) {
            closing_for_restart_ = false;
            return;
        }

        // Unexpected close (network flap, remote close, etc.)
        restartSync();
    }

    void GenericFeedHandler::schedule_ws_reconnect_(std::chrono::milliseconds delay) {
        ++reconnect_gen_;
        const auto my_gen = reconnect_gen_;

        reconnect_scheduled_ = true;
        reconnect_timer_.expires_after(delay);

        reconnect_timer_.async_wait([this, my_gen](const boost::system::error_code &ec) {
            if (ec) return;
            if (!running_.load()) return;
            if (my_gen != reconnect_gen_) return;

            reconnect_scheduled_ = false;

            if (rt_.caps.requires_ws_bootstrap) {
                state_ = SyncState::BOOTSTRAPPING;
                bootstrapWS();
            } else {
                state_ = SyncState::CONNECTING;
                connectWS();
            }
        });
    }
}
