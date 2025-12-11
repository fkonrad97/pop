#pragma once    /// Ensures this header file is only included once per translation unit.

#include <boost/asio/io_context.hpp>    /// Core event loop for all async I/O.
#include <boost/asio/ip/tcp.hpp>        /// TCP socket and resolver primitives.
#include <boost/beast/websocket.hpp>    /// WebSocket client/server over Asio.
#include <boost/beast/ssl.hpp>          /// SSL/TLS stream wrappers for Beast.
#include <boost/beast/core.hpp>         /// Core utilities (buffers, error handling).
#include <boost/asio/ssl/context.hpp>   /// ssl::context for TLS configuration.
#include <functional>                   /// std::function for user callbacks.
#include <string>

namespace md {

    /**
     * @class WsClient
     * @brief Minimal async WSS client (WebSocket over TLS) built on Boost.Asio + Boost.Beast.
     *
     * Design:
     *   - One instance models one logical WS connection:
     *       resolve → TCP connect → TLS handshake → WS handshake → read loop → close
     *   - Uses enable_shared_from_this so pending async ops keep the object alive.
     *   - Users provide:
     *       - on_message_ (string frames, typically JSON from venue)
     *       - on_close_   (terminal event: graceful/abnormal close or error path)
     *
     * Ownership & Lifetime:
     *   - Constructed with a reference to a process-level io_context (owned elsewhere).
     *   - Holds a resolver, an ssl-wrapped websocket stream, and a read buffer.
     *   - Close path sends WS close, then performs TLS shutdown (server-dependent).
     *
     * Threading:
     *   - Not thread-safe by itself. If used from multiple threads, run it on a strand.
     *
     * Notes:
     *   - SNI and certificate verification must be configured before TLS handshake.
     *   - Per-message deflate can be enabled during WS handshake if the venue supports it.
     */
    class WsClient : public std::enable_shared_from_this<WsClient> {
    public:
        /// raw view over the frame bytes (no allocations)
        using RawMessageHandler = std::function<void(const char*, std::size_t)>;

        /// Called when the connection closes or any terminal error occurs (exactly once).
        using CloseHandler   = std::function<void()>;

        using OpenHandler    = std::function<void()>; 

        /**
         * @brief Bind the client to the external event loop.
         * @param ioc The shared process-level io_context that drives all async I/O.
         *
         * TLS context is created as tls_client; you can further configure verify mode/paths.
         */
        explicit WsClient(boost::asio::io_context &ioc);

        /**
         * @brief Begin the async connect chain.
         *
         * Steps (all async):
         *   1) do_resolve_(host, port)         : DNS lookup
         *   2) do_tcp_connect_(results)                : connect TCP
         *   3) do_tls_handshake_()                     : client TLS handshake (SNI, verify)
         *   4) do_ws_handshake_()                      : WebSocket handshake (Host + target)
         *   5) do_read_()                              : start perpetual read loop
         *
         * @param host   e.g. "stream.binance.com"
         * @param port   e.g. "9443"
         * @param target e.g. "/ws/btcusdt@aggTrade"
         */
        void connect(const std::string &host,
                     const std::string &port,
                     const std::string &target);

        /// To be used by feed handlers.
        void set_on_raw_message(RawMessageHandler h);

        /// Attach a user handler for close/loss-of-connection notification.
        void set_on_close(CloseHandler h);

        void set_on_open(OpenHandler h);

        void send_text(const std::string& text);

        /**
         * @brief Initiate a graceful shutdown sequence.
         *
         * Sequence:
         *   - send WebSocket close frame
         *   - allow server to echo and close
         *   - perform TLS shutdown (some servers simply close; handle EOF gracefully)
         *
         * Idempotent: safe to call once; subsequent calls no-op while closing_ is true.
         */
        void close();

    private:
        // =====================
        // Shared state
        // =====================

        /// Reference to the external event loop (owned by the process).
        boost::asio::io_context &ioc_;

        /// TLS configuration (verify mode, CA bundle, etc.). Constructed in client mode.
        boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_client};

        /**
         * The composed stream stack:
         *   tcp::socket → ssl_stream<tcp::socket> → websocket::stream<ssl_stream<>>
         *
         * We speak WebSocket (text/binary frames) over a secure TLS tunnel.
         */
        boost::beast::websocket::stream<
            boost::beast::ssl_stream<boost::asio::ip::tcp::socket>> ws_{ioc_, ssl_ctx_};

        /// DNS resolver bound to the same io_context.
        boost::asio::ip::tcp::resolver resolver_{ioc_};

        /// Read buffer used by Beast for message frames.
        boost::beast::flat_buffer buffer_;

        /// Cached request params for reconnect or diagnostics.
        std::string host_;
        std::string target_;

        /// Guard to avoid double-close / re-entrancy on error paths.
        bool closing_ = false;

        /// User-supplied callbacks
        CloseHandler   on_close_;
        RawMessageHandler on_raw_message_;
        OpenHandler   on_open_;

        // =====================
        // Chain step helpers
        // =====================

        /**
         * @brief Resolve DNS (host_:port).
         * Next: do_tcp_connect_(results)
         *
         * Also stores host_/target_ for SNI and WS Host header.
         */
        void do_resolve_(const std::string& host, const std::string& port);

        /**
         * @brief Establish TCP connection to one of the resolved endpoints.
         * Next: do_tls_handshake_()
         */
        void do_tcp_connect_(const boost::asio::ip::tcp::resolver::results_type& results);

        /**
         * @brief Perform client TLS handshake on the underlying ssl_stream.
         * Next: do_ws_handshake_()
         *
         * Implementation checklist:
         *  - Set SNI host: SSL_set_tlsext_host_name(ws_.next_layer().native_handle(), host_.c_str())
         *  - Configure ssl_ctx_ verification (set_default_verify_paths / load_verify_file + verify_peer).
         */
        void do_tls_handshake_();

        /**
         * @brief Perform the HTTP → WS upgrade handshake.
         * Next: do_read_() loop
         *
         * Typical settings (in implementation):
         *  - ws_.set_option(websocket::stream_base::timeout::suggested(role_type::client))
         *  - ws_.set_option(websocket::stream_base::decorator([](request_type& req){ req.set(field::user_agent, "..."); }))
         *  - ws_.text(true) if you expect textual JSON frames (default often OK).
         *  - Optionally negotiate permessage-deflate if supported by venue.
         */
        void do_ws_handshake_();

        /**
         * @brief Perpetual async read loop: reads one message into `buffer_`,
         *        forwards as string to `on_message_`, then issues the next async_read.
         *
         * Exit conditions:
         *  - Remote sends close: triggers close path and on_close_.
         *  - Any error: triggers on_close_ (once) and stops reading.
         */
        void do_read_();
    };

} // namespace md