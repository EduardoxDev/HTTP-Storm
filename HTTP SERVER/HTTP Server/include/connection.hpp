#pragma once

#include "http_parser.hpp"
#include "tls_context.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace hphttp {

enum class ConnectionState {
    READING,
    PROCESSING,
    WRITING,
    KEEP_ALIVE_WAIT,
    CLOSING,
    CLOSED
};

class Connection : public std::enable_shared_from_this<Connection> {
public:
    using RequestCallback = std::function<HttpResponse(const HttpRequest&)>;

    Connection(int fd,
               std::string remote_addr,
               RequestCallback on_request,
               TlsContext* tls_context = nullptr,
               int keepalive_timeout_ms = 75000,
               int keepalive_max_requests = 1000,
               std::size_t max_request_size = 1048576);

    ~Connection();

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    void handle_read();
    void handle_write();
    void close();

    [[nodiscard]] int                     fd() const noexcept;
    [[nodiscard]] const std::string&      remote_addr() const noexcept;
    [[nodiscard]] ConnectionState         state() const noexcept;
    [[nodiscard]] bool                    is_alive() const noexcept;
    [[nodiscard]] bool                    is_timed_out() const noexcept;
    [[nodiscard]] std::chrono::steady_clock::time_point last_activity() const noexcept;

    void reset_timeout();

private:
    bool  do_tls_handshake();
    bool  read_data(std::vector<char>& buffer);
    bool  write_data();
    void  prepare_response(HttpResponse response);
    void  update_activity();

    int                   fd_;
    std::string           remote_addr_;
    RequestCallback       on_request_;
    TlsContext*           tls_context_;
    SslPtr                ssl_;

    HttpParser            parser_;
    ConnectionState       state_{ConnectionState::READING};
    std::vector<char>     write_buffer_;
    std::size_t           write_offset_{0};

    int                   keepalive_timeout_ms_;
    int                   keepalive_max_requests_;
    std::size_t           max_request_size_;
    int                   requests_handled_{0};
    bool                  tls_handshake_done_{false};

    std::chrono::steady_clock::time_point last_activity_;
    std::chrono::steady_clock::time_point connect_time_;
};

}
