#include "ws_client.hpp"

#include <boost/beast/core.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/connect.hpp>
#include <openssl/ssl.h> // for SSL_set_tlsext_host_name
#include <iostream>

namespace md
{
    using tcp = boost::asio::ip::tcp;
    namespace beast = boost::beast;
    namespace websocket = beast::websocket;
    namespace ssl = boost::asio::ssl;

    WsClient::WsClient(boost::asio::io_context &ioc)
        : ioc_(ioc),
          ssl_ctx_(ssl::context::tls_client),
          ws_(ioc_, ssl_ctx_),
          resolver_(ioc_)
    {
        /// note: must be constructed with ioc
        /// TLS defaults (you can load CA bundle explicitly if you want)
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);

        /// Reasonable timeouts for clients
        ws_.set_option(websocket::stream_base::timeout::suggested(beast::role_type::client));

        /// user-agent header
        ws_.set_option(websocket::stream_base::decorator(
            [](websocket::request_type &req)
            {
                req.set(beast::http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " pop-wsclient");
            }));
    }

    void WsClient::set_on_raw_message(RawMessageHandler h)
    {
        on_raw_message_ = std::move(h);
    }
    void WsClient::set_on_close(CloseHandler h) { on_close_ = std::move(h); }

    void WsClient::set_on_open(OpenHandler h)
    {
        on_open_ = std::move(h);
    }

    void WsClient::send_text(const std::string &text)
    {
        auto self = shared_from_this();

        ws_.async_write(
            boost::asio::buffer(text),
            [self](const beast::error_code &ec, std::size_t /*bytes_transferred*/)
            {
                if (ec)
                {
                    std::cerr << "[WsClient] write error: " << ec.message() << "\n";
                    if (self->on_close_)
                    {
                        self->on_close_();
                    }
                }
            });
    }

    void WsClient::connect(const std::string &host,
                           const std::string &port,
                           const std::string &target)
    {
        /// store for potential reuse
        host_ = host;
        target_ = target;

        /// kick off the async chain
        do_resolve_(host, port);
    }

    void WsClient::do_resolve_(const std::string &host, const std::string &port)
    {
        auto self = shared_from_this();
        resolver_.async_resolve(
            host,
            port,
            [self](const beast::error_code &ec, const tcp::resolver::results_type &results)
            {
                if (ec)
                {
                    std::cerr << "[WsClient] resolve error: " << ec.message() << "\n";
                    if (self->on_close_)
                        self->on_close_();
                    return;
                }
                self->do_tcp_connect_(results);
            });
    }

    void WsClient::do_tcp_connect_(const tcp::resolver::results_type &results)
    {
        auto self = shared_from_this();

        // function async_connect for a range of endpoints
        boost::asio::async_connect(
            boost::beast::get_lowest_layer(ws_), // tcp::socket&
            results,
            [self](const boost::system::error_code &ec, const tcp::endpoint &)
            {
                if (ec)
                {
                    std::cerr << "[WsClient] tcp connect error: " << ec.message() << "\n";
                    if (self->on_close_)
                        self->on_close_();
                    return;
                }

                // Set SNI host name (required by many hosts, incl. Binance)
                if (!SSL_set_tlsext_host_name(self->ws_.next_layer().native_handle(),
                                              self->host_.c_str()))
                {
                    const boost::system::error_code sni_ec{
                        static_cast<int>(::ERR_get_error()),
                        boost::asio::error::get_ssl_category()};
                    std::cerr << "[WsClient] SNI error: " << sni_ec.message() << "\n";
                    if (self->on_close_)
                        self->on_close_();
                    return;
                }

                self->do_tls_handshake_();
            });
    }

    void WsClient::do_tls_handshake_()
    {
        auto self = shared_from_this();

        ws_.next_layer().async_handshake(ssl::stream_base::client,
                                         [self](const beast::error_code &ec)
                                         {
                                             if (ec)
                                             {
                                                 std::cerr << "[WsClient] TLS handshake error: " << ec.message() << "\n";
                                                 if (self->on_close_)
                                                     self->on_close_();
                                                 return;
                                             }
                                             self->do_ws_handshake_();
                                         });
    }

    void WsClient::do_ws_handshake_()
    {
        auto self = shared_from_this();

        // e.g.: Binance expects Host header = host_ and target_ like "/ws/btcusdt@depth"
        ws_.async_handshake(host_, target_,
                            [self](const beast::error_code &ec)
                            {
                                if (ec)
                                {
                                    std::cerr << "[WsClient] WS handshake error: " << ec.message() << "\n";
                                    if (self->on_close_)
                                        self->on_close_();
                                    return;
                                }

                                // Text mode: Binance typically sends JSON frames
                                self->ws_.text(true);

                                // notify "open"
                                if (self->on_open_)
                                {
                                    self->on_open_();
                                }

                                // start reading frames
                                self->do_read_();
                            });
    }

    void WsClient::do_read_()
    {
        auto self = shared_from_this();

        ws_.async_read(
            buffer_,
            [self](const beast::error_code &ec, std::size_t bytes_transferred)
            {
                if (ec)
                {
                    if (ec == websocket::error::closed)
                    {
                        if (self->on_close_)
                            self->on_close_();
                        return;
                    }
                    std::cerr << "[WsClient] read error: " << ec.message() << "\n";
                    if (self->on_close_)
                        self->on_close_();
                    return;
                }

                auto seq = self->buffer_.data();
                const char *ptr = static_cast<const char *>(seq.data());
                std::size_t len = bytes_transferred;

                if (self->on_raw_message_)
                {
                    self->on_raw_message_(ptr, len);
                }

                self->buffer_.consume(len);
                self->do_read_();
            });
    }

    void WsClient::close()
    {
        // post ensures we run on the io_context thread
        auto self = shared_from_this();

        boost::asio::dispatch(ioc_, [self]
                              {
            if (self->closing_) return;
            self->closing_ = true;

            // Graceful close: send close frame, then TLS shutdown happens under the hood
            self->ws_.async_close(websocket::close_code::normal,
                                  [self](const beast::error_code &ec) {
                                      if (ec) {
                                          // If already closed or shutdown, just notify and exit
                                          std::cerr << "[WsClient] close error: " << ec.message() << "\n";
                                      }
                                      if (self->on_close_) self->on_close_();
                                  }
            ); });
    }
} // namespace md
