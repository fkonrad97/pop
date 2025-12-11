#include "../../include/client_connection_handlers/RestClient.hpp"

#include <boost/asio/connect.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ssl/error.hpp>
#include <openssl/ssl.h>   // SSL_set_tlsext_host_name
#include <iostream>

namespace md {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ssl = boost::asio::ssl;
    using tcp = boost::asio::ip::tcp;

    RestClient::RestClient(boost::asio::io_context &ioc)
        : ioc_(ioc),
          ssl_ctx_(ssl::context::tls_client),
          resolver_(ioc_),
          stream_(ioc_, ssl_ctx_) {
        // TLS defaults: verify peers using system CAs
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);
    }

    void RestClient::async_get(std::string host, std::string port, std::string target, ResponseHandler cb) {
        host_ = std::move(host);
        target_ = std::move(target);
        port_ = std::move(port);
        cb_ = std::move(cb);

        // Prepare HTTP GET request now (Host, UA set here; path set via target_)
        req_.version(11);
        req_.method(http::verb::get);
        req_.target(target_);
        req_.set(http::field::host, host_);
        req_.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " pop-restclient");

        do_resolve_();
    }

    void RestClient::async_post(std::string host,
                                std::string port,
                                std::string target,
                                std::string body,
                                ResponseHandler cb) {
        host_ = std::move(host);
        target_ = std::move(target);
        port_ = std::move(port);
        cb_ = std::move(cb);

        // Build HTTP POST request
        req_.version(11);
        req_.method(boost::beast::http::verb::post);
        req_.target(target_);
        req_.set(boost::beast::http::field::host, host_);
        req_.set(boost::beast::http::field::user_agent, "pop-restclient");
        req_.set(boost::beast::http::field::content_type, "application/json");
        req_.body() = std::move(body);
        req_.prepare_payload(); // sets Content-Length

        do_resolve_();
    }

    void RestClient::do_resolve_() {
        auto self = shared_from_this();
        resolver_.async_resolve(
            host_, port_,
            [self](const boost::system::error_code &ec, const tcp::resolver::results_type &results) {
                if (ec) {
                    if (self->cb_) self->cb_(ec, {});
                    return;
                }
                self->do_tcp_connect_(results);
            }
        );
    }

    void RestClient::do_tcp_connect_(const tcp::resolver::results_type &results) {
        auto self = shared_from_this();
        boost::asio::async_connect(
            beast::get_lowest_layer(stream_), results,
            [self](const boost::system::error_code &ec, const tcp::endpoint &) {
                if (ec) {
                    if (self->cb_) self->cb_(ec, {});
                    return;
                }

                // Set SNI host name (required by many hosts)
                if (!SSL_set_tlsext_host_name(self->stream_.native_handle(), self->host_.c_str())) {
                    const boost::system::error_code sni_ec{
                        static_cast<int>(::ERR_get_error()),
                        boost::asio::error::get_ssl_category()
                    };
                    if (self->cb_) self->cb_(sni_ec, {});
                    return;
                }

                self->do_tls_handshake_();
            }
        );
    }

    void RestClient::do_tls_handshake_() {
        auto self = shared_from_this();
        stream_.async_handshake(ssl::stream_base::client,
                                [self](const boost::system::error_code &ec) {
                                    if (ec) {
                                        if (self->cb_) self->cb_(ec, {});
                                        return;
                                    }
                                    self->do_http_request_();
                                }
        );
    }

    void RestClient::do_http_request_() {
        auto self = shared_from_this();
        http::async_write(stream_, req_,
                          [self](const boost::system::error_code &ec, std::size_t) {
                              if (ec) {
                                  if (self->cb_) self->cb_(ec, {});
                                  return;
                              }
                              self->do_http_read_();
                          }
        );
    }

    void RestClient::do_http_read_() {
        auto self = shared_from_this();
        http::async_read(stream_, buffer_, res_,
                         [self](const boost::system::error_code &ec, std::size_t) {
                             if (ec) {
                                 if (self->cb_) self->cb_(ec, {});
                                 return;
                             }

                             // Success: deliver body to caller
                             const std::string body = std::move(self->res_.body());
                             if (self->cb_) self->cb_({}, body);

                             // Then gracefully shut down TLS
                             self->do_tls_shutdown_();
                         }
        );
    }

    void RestClient::do_tls_shutdown_() {
        if (shutting_down_) return;
        shutting_down_ = true;

        auto self = shared_from_this();
        stream_.async_shutdown(
            [](const boost::system::error_code &ec) {
                // OpenSSL often returns EOF/stream_truncated on shutdown; it's fine to ignore.
                if (!ec) {
                    return;
                }

                // Treat EOF and stream_truncated as non-errors
                if (ec == boost::asio::error::eof ||
                    ec == boost::asio::ssl::error::stream_truncated) {
                    return;
                }

                std::cerr << "[RESTCLIENT] TLS shutdown error: "
                        << ec.message() << "\n";
            }
        );
    }
} // namespace md
