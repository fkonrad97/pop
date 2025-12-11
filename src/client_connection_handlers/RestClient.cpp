#include "client_connection_handlers/RestClient.hpp"

#include <boost/asio/connect.hpp>
#include <boost/beast/version.hpp>
#include <boost/asio/ssl/error.hpp>
#include <openssl/ssl.h>
#include <iostream>

namespace md {
    namespace beast = boost::beast;
    namespace http = beast::http;
    namespace ssl = boost::asio::ssl;
    using tcp = boost::asio::ip::tcp;

    RestClient::RestClient(boost::asio::io_context &ioc)
        : ioc_(ioc),
          ssl_ctx_(ssl::context::tls_client),
          resolver_(ioc_) {
        ssl_ctx_.set_default_verify_paths();
        ssl_ctx_.set_verify_mode(ssl::verify_peer);

        // stream_ constructed lazily per request
    }

    void RestClient::reset_per_request_state_() {
        shutting_down_ = false;
        final_ec_.clear();
        response_body_.clear();

        buffer_.consume(buffer_.size());
        res_ = {};
        req_ = {};

        // Fresh ssl_stream every request => clean OpenSSL state
        stream_ = std::make_unique<beast::ssl_stream<tcp::socket> >(ioc_, ssl_ctx_);
    }

    void RestClient::async_get(std::string host, std::string target, std::string port, ResponseHandler cb) {
        if (in_flight_.exchange(true)) {
            if (cb) cb(make_error_code(boost::system::errc::operation_in_progress), {});
            return;
        }

        host_ = std::move(host);
        target_ = std::move(target);
        port_ = std::move(port);
        cb_ = std::move(cb);

        reset_per_request_state_();

        req_.version(11);
        req_.method(http::verb::get);
        req_.target(target_);
        req_.set(http::field::host, host_);
        req_.set(http::field::user_agent, std::string(BOOST_BEAST_VERSION_STRING) + " pop-restclient");
        req_.set(http::field::connection, "close"); // keep it simple/robust

        do_resolve_();
    }

    void RestClient::async_post(std::string host, std::string port, std::string target, std::string body,
                                ResponseHandler cb) {
        if (in_flight_.exchange(true)) {
            if (cb) cb(make_error_code(boost::system::errc::operation_in_progress), {});
            return;
        }

        host_ = std::move(host);
        target_ = std::move(target);
        port_ = std::move(port);
        cb_ = std::move(cb);

        reset_per_request_state_();

        req_.version(11);
        req_.method(http::verb::post);
        req_.target(target_);
        req_.set(http::field::host, host_);
        req_.set(http::field::user_agent, "pop-restclient");
        req_.set(http::field::content_type, "application/json");
        req_.set(http::field::connection, "close");

        req_.body() = std::move(body);
        req_.prepare_payload();

        do_resolve_();
    }

    void RestClient::fail_(boost::system::error_code ec) {
        if (final_ec_) return; // first error wins
        final_ec_ = ec;
        do_tls_shutdown_(); // best-effort cleanup, then finish_()
    }

    void RestClient::finish_() {
        // Ensure we only call cb_ once and allow re-entry safely
        in_flight_.store(false);

        auto cb = std::move(cb_);
        cb_ = {};

        const auto ec = final_ec_;
        auto body = std::move(response_body_);

        if (cb) cb(ec, body);
    }

    void RestClient::do_resolve_() {
        auto self = shared_from_this();
        resolver_.async_resolve(
            host_, port_,
            [self](const boost::system::error_code &ec, const tcp::resolver::results_type &results) {
                if (ec) return self->fail_(ec);
                self->do_tcp_connect_(results);
            }
        );
    }

    void RestClient::do_tcp_connect_(const tcp::resolver::results_type &results) {
        auto self = shared_from_this();

        // Optional: timeouts on the underlying socket layer
        // beast::get_lowest_layer(*stream_).expires_after(std::chrono::seconds(10));

        boost::asio::async_connect(
            beast::get_lowest_layer(*stream_), results,
            [self](const boost::system::error_code &ec, const tcp::endpoint &) {
                if (ec) return self->fail_(ec);

                // SNI
                if (!SSL_set_tlsext_host_name(self->stream_->native_handle(), self->host_.c_str())) {
                    const boost::system::error_code sni_ec{
                        static_cast<int>(::ERR_get_error()),
                        boost::asio::error::get_ssl_category()
                    };
                    return self->fail_(sni_ec);
                }

                self->do_tls_handshake_();
            }
        );
    }

    void RestClient::do_tls_handshake_() {
        auto self = shared_from_this();
        stream_->async_handshake(ssl::stream_base::client,
                                 [self](const boost::system::error_code &ec) {
                                     if (ec) return self->fail_(ec);
                                     self->do_http_request_();
                                 }
        );
    }

    void RestClient::do_http_request_() {
        auto self = shared_from_this();
        http::async_write(*stream_, req_,
                          [self](const boost::system::error_code &ec, std::size_t) {
                              if (ec) return self->fail_(ec);
                              self->do_http_read_();
                          }
        );
    }

    void RestClient::do_http_read_() {
        auto self = shared_from_this();
        http::async_read(*stream_, buffer_, res_,
                         [self](const boost::system::error_code &ec, std::size_t) {
                             if (ec) return self->fail_(ec);

                             // Success path
                             self->response_body_ = std::move(self->res_.body());
                             self->final_ec_.clear();

                             self->do_tls_shutdown_();
                         }
        );
    }

    void RestClient::do_tls_shutdown_() {
        if (shutting_down_) return;
        shutting_down_ = true;

        auto self = shared_from_this();
        stream_->async_shutdown(
            [self](const boost::system::error_code &ec) {
                // Ignore common shutdown “errors”
                if (ec &&
                    ec != boost::asio::error::eof &&
                    ec != boost::asio::ssl::error::stream_truncated) {
                    std::cerr << "[RESTCLIENT] TLS shutdown error: " << ec.message() << "\n";
                }

                // Always close the socket
                boost::system::error_code ignored;
                auto &sock = beast::get_lowest_layer(*self->stream_);
                sock.shutdown(tcp::socket::shutdown_both, ignored);
                sock.close(ignored);

                self->finish_();
            }
        );
    }
} // namespace md
