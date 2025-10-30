#pragma once

#include <boost/asio/io_context.hpp>    /// Core event loop that drives all async operations.
#include <boost/asio/ip/tcp.hpp>        /// TCP primitives (socket, resolver).
#include <boost/asio/ssl/context.hpp>   /// SSL/TLS configuration container (certs, verify mode, etc.).
#include <boost/beast/core.hpp>         /// Beast core utilities (buffers, error handling).
#include <boost/beast/http.hpp>         /// Beast HTTP message types and algorithms.
#include <boost/beast/ssl.hpp>          /// SSL stream wrapper that composes with Asio sockets.
#include <functional>                   /// std::function for user-provided callbacks.
#include <string>

namespace md {

    /**
     * @class RestClient
     *
     * @brief Minimal async HTTPS (HTTP/1.1) client built on Boost.Asio + Boost.Beast.
     *
     * Design:
     *  - One client instance performs one logical request chain at a time:
     *        resolve DNS → TCP connect → TLS handshake → HTTP request → HTTP read → TLS shutdown
     *  - Uses `std::enable_shared_from_this` so pending async ops keep the object alive.
     *  - The user supplies a completion callback receiving `(error_code, response_body)`.
     *
     * Ownership & Lifetime:
     *  - Construct with a reference to a process-level `io_context` (owned elsewhere).
     *  - Internally holds a resolver, an SSL stream (TCP socket wrapped in TLS), and Beast HTTP buffers/messages.
     *  - The last async step (`do_tls_shutdown_`) will invoke the user callback (exactly once).
     *
     * Threading:
     *  - Typically, the `io_context` is driven by one thread per process (or a small pool).
     *  - This class is not thread-safe by itself; use it from the same strand/execution context if needed.
     */
    class RestClient : public std::enable_shared_from_this<RestClient> {
    public:
        /// Completion signature: (ec, body). On error, body is usually empty.
        using ResponseHandler = std::function<void(boost::system::error_code, const std::string&)>;

        /**
         * @brief Create a REST client bound to the given io_context.
         *
         * @param ioc External io_context that drives all async I/O in this process (shared across clients).
         *
         * Note: The SSL/TLS context is created with `tls_client` mode by default.
         *       You may additionally configure `ssl_ctx_` (verify mode, CA bundle, etc.) before use.
         */
        explicit RestClient(boost::asio::io_context &ioc);

        /**
         * @brief Start an asynchronous HTTPS GET request.
         *
         * Chain (all async):
         *   1) do_resolve_()         : DNS resolve (host:port) → results
         *   2) do_tcp_connect_()     : connect TCP socket to one endpoint
         *   3) do_tls_handshake_()   : establish TLS tunnel (client)
         *   4) do_http_request_()    : write HTTP/1.1 GET
         *   5) do_http_read_()       : read HTTP response into `res_`
         *   6) do_tls_shutdown_()    : attempt orderly TLS shutdown, then invoke user callback
         *
         * @param host   e.g. "api.binance.com"
         * @param target e.g. "/api/v3/time" (must start with '/')
         * @param port   e.g. "443"
         * @param cb     User callback notified upon completion or error.
         *
         * Remarks:
         *  - SNI: In the implementation, remember to set SNI host name on the SSL stream for many providers.
         *  - Verification: Set `ssl_ctx_.set_verify_mode(ssl::verify_peer)` and the CA bundle before use in production.
         *  - Reuse: This object stores host/target/port so you can trigger another call after completion if desired.
         */
        void async_get(std::string host,
                       std::string target,
                       std::string port,
                       ResponseHandler cb);

    private:
        // =====================
        // Chain step helpers
        // =====================

        /**
         * @brief Resolve hostname to endpoints.
         * Input:  host_ + port_
         * Output: resolver results → next: do_tcp_connect_(results)
         */
        void do_resolve_();

        /**
         * @brief Connect to one of the resolved endpoints.
         * Input:  resolver::results_type
         * Output: established TCP connection → next: do_tls_handshake_()
         */
        void do_tcp_connect_(const boost::asio::ip::tcp::resolver::results_type& results);

        /**
         * @brief Perform the client-side TLS handshake over the connected TCP socket.
         * Output: secure channel ready → next: do_http_request_()
         *
         * Note: Proper SNI and verify settings should be configured before this step.
         */
        void do_tls_handshake_();

        /**
         * @brief Write the HTTP/1.1 GET request (`req_`) to the TLS stream.
         * Output: request sent → next: do_http_read_()
         *
         * `req_` typically has:
         *   - method: GET
         *   - target: target_
         *   - version: 11 (HTTP/1.1)
         *   - headers: Host, User-Agent, (optional) Connection: close
         */
        void do_http_request_();

        /**
         * @brief Read the HTTP response into `res_`.
         * Output: response complete → next: do_tls_shutdown_()
         *
         * On success, `res_.body()` contains the response payload (string_body).
         * Errors (including HTTP status != 200) should be propagated via the callback with a suitable error_code.
         */
        void do_http_read_();

        /**
         * @brief Attempt an orderly TLS shutdown (send/receive close_notify).
         * Output: invokes user callback exactly once with (ec, body)
         *
         * Notes:
         *  - Some servers close the connection without proper TLS shutdown.
         *    Handle EOF and short read cases gracefully (often not fatal after a full HTTP read).
         *  - Guard with `shutting_down_` to avoid duplicate callbacks if upstream errors occur.
         */
        void do_tls_shutdown_();

        // =====================
        // Shared state
        // =====================

        /// Reference to the shared event loop (owned by the process).
        boost::asio::io_context &ioc_;

        /// TLS configuration container (verify mode, CA certs, client certs, etc.).
        /// Constructed in client mode: ssl::context::tls_client
        boost::asio::ssl::context ssl_ctx_{boost::asio::ssl::context::tls_client};

        /// DNS resolver bound to the same io_context.
        boost::asio::ip::tcp::resolver resolver_{ioc_};

        /**
         * @brief The actual I/O stream:
         *   tcp::socket  → wrapped by → ssl_stream<tcp::socket>
         *
         * We do plain HTTP over this secure channel using Beast algorithms.
         */
        boost::beast::ssl_stream<boost::asio::ip::tcp::socket> stream_{ioc_, ssl_ctx_};

        /// Temporary buffer used by Beast for reads (HTTP response parsing).
        boost::beast::flat_buffer buffer_;

        /// Outgoing HTTP request (string body; for GET usually empty body).
        boost::beast::http::request<boost::beast::http::string_body> req_;

        /// Incoming HTTP response (string body).
        boost::beast::http::response<boost::beast::http::string_body> res_;

        /// Request parameters captured for the chain.
        std::string host_;    ///< e.g., "api.binance.com"
        std::string target_;  ///< e.g., "/api/v3/time"
        std::string port_;    ///< e.g., "443"

        /// User-provided completion callback.
        ResponseHandler cb_;

        /// Prevent double shutdown/callback on racey error paths.
        bool shutting_down_ = false;
    };

} // namespace md
