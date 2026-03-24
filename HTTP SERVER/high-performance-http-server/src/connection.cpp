#include "connection.hpp"
#include "logger.hpp"

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

namespace hphttp {

Connection::Connection(int fd,
                       std::string remote_addr,
                       RequestCallback on_request,
                       TlsContext* tls_context,
                       int keepalive_timeout_ms,
                       int keepalive_max_requests,
                       std::size_t max_request_size)
    : fd_(fd)
    , remote_addr_(std::move(remote_addr))
    , on_request_(std::move(on_request))
    , tls_context_(tls_context)
    , keepalive_timeout_ms_(keepalive_timeout_ms)
    , keepalive_max_requests_(keepalive_max_requests)
    , max_request_size_(max_request_size)
    , last_activity_(std::chrono::steady_clock::now())
    , connect_time_(std::chrono::steady_clock::now())
{
    if (tls_context_) {
        ssl_ = tls_context_->create_ssl(fd_);
    }
}

Connection::~Connection() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

void Connection::handle_read() {
    if (state_ == ConnectionState::CLOSED || state_ == ConnectionState::CLOSING) return;

    if (tls_context_ && !tls_handshake_done_) {
        if (!do_tls_handshake()) return;
    }

    update_activity();

    std::vector<char> buf(65536);
    if (!read_data(buf)) {
        close();
        return;
    }
}

void Connection::handle_write() {
    if (state_ != ConnectionState::WRITING) return;

    update_activity();

    if (!write_data()) {
        close();
        return;
    }

    if (write_offset_ >= write_buffer_.size()) {
        write_buffer_.clear();
        write_offset_ = 0;

        bool should_keep_alive = (requests_handled_ < keepalive_max_requests_);
        if (should_keep_alive && parser_.is_complete() && parser_.request().keep_alive) {
            parser_.reset();
            state_ = ConnectionState::READING;
        } else {
            close();
        }
    }
}

void Connection::close() {
    if (state_ == ConnectionState::CLOSED) return;

    if (ssl_) {
        SSL_shutdown(ssl_.get());
        ssl_.reset();
    }

    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }

    state_ = ConnectionState::CLOSED;
}

int Connection::fd() const noexcept {
    return fd_;
}

const std::string& Connection::remote_addr() const noexcept {
    return remote_addr_;
}

ConnectionState Connection::state() const noexcept {
    return state_;
}

bool Connection::is_alive() const noexcept {
    return state_ != ConnectionState::CLOSED && state_ != ConnectionState::CLOSING && fd_ >= 0;
}

bool Connection::is_timed_out() const noexcept {
    auto now     = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_activity_).count();
    return elapsed > keepalive_timeout_ms_;
}

std::chrono::steady_clock::time_point Connection::last_activity() const noexcept {
    return last_activity_;
}

void Connection::reset_timeout() {
    update_activity();
}

bool Connection::do_tls_handshake() {
    int result = SSL_accept(ssl_.get());
    if (result == 1) {
        tls_handshake_done_ = true;
        return true;
    }

    int err = SSL_get_error(ssl_.get(), result);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return false;
    }

    Logger::instance().error("TLS handshake failed for {}: {}",
                              remote_addr_, TlsContext::last_error_string());
    close();
    return false;
}

bool Connection::read_data(std::vector<char>& buffer) {
    while (true) {
        ssize_t n = 0;

        if (ssl_) {
            n = SSL_read(ssl_.get(), buffer.data(), static_cast<int>(buffer.size()));
            if (n <= 0) {
                int err = SSL_get_error(ssl_.get(), static_cast<int>(n));
                if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) return true;
                if (err == SSL_ERROR_ZERO_RETURN) return false;
                return false;
            }
        } else {
            n = ::recv(fd_, buffer.data(), buffer.size(), MSG_NOSIGNAL);
            if (n == 0) return false;
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                return false;
            }
        }

        auto result = parser_.feed(std::string_view(buffer.data(), static_cast<std::size_t>(n)));

        if (result == HttpParser::FeedResult::ERROR) {
            HttpResponse resp;
            resp.status_code    = 400;
            resp.status_message = "Bad Request";
            resp.set_json_body(R"({"error":"Bad Request","code":400})");
            prepare_response(std::move(resp));
            return true;
        }

        if (result == HttpParser::FeedResult::COMPLETE) {
            state_ = ConnectionState::PROCESSING;
            HttpRequest req = parser_.request();
            req.remote_addr = remote_addr_;

            ++requests_handled_;

            HttpResponse resp = on_request_(req);

            resp.set_header("Server",     "hphttp/1.0");
            resp.set_header("Connection", req.keep_alive ? "keep-alive" : "close");

            if (!resp.headers.count("Content-Length") && !resp.compressed) {
                resp.set_header("Content-Length", std::to_string(resp.body.size()));
            }

            prepare_response(std::move(resp));
            return true;
        }

        if (parser_.request().body.size() + static_cast<std::size_t>(n) > max_request_size_) {
            HttpResponse resp;
            resp.status_code    = 413;
            resp.status_message = "Payload Too Large";
            resp.set_json_body(R"({"error":"Payload Too Large","code":413})");
            prepare_response(std::move(resp));
            return true;
        }
    }
}

bool Connection::write_data() {
    while (write_offset_ < write_buffer_.size()) {
        ssize_t n = 0;
        const char* ptr  = write_buffer_.data() + write_offset_;
        std::size_t size = write_buffer_.size() - write_offset_;

        if (ssl_) {
            n = SSL_write(ssl_.get(), ptr, static_cast<int>(size));
            if (n <= 0) {
                int err = SSL_get_error(ssl_.get(), static_cast<int>(n));
                if (err == SSL_ERROR_WANT_WRITE || err == SSL_ERROR_WANT_READ) return true;
                return false;
            }
        } else {
            n = ::send(fd_, ptr, size, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                return false;
            }
        }

        write_offset_ += static_cast<std::size_t>(n);
    }

    return true;
}

void Connection::prepare_response(HttpResponse response) {
    std::string serialized = response.serialize();
    write_buffer_.assign(serialized.begin(), serialized.end());
    write_offset_ = 0;
    state_        = ConnectionState::WRITING;
    handle_write();
}

void Connection::update_activity() {
    last_activity_ = std::chrono::steady_clock::now();
}

}
