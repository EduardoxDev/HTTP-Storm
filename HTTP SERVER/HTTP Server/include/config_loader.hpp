#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace hphttp {

struct TlsConfig {
    bool        enabled{false};
    std::string cert_file;
    std::string key_file;
    std::string ca_file;
    std::string ciphers{"ECDHE-ECDSA-AES256-GCM-SHA384:ECDHE-RSA-AES256-GCM-SHA384"};
    bool        verify_client{false};
};

struct RateLimitConfig {
    bool        enabled{false};
    std::size_t requests_per_second{1000};
    std::size_t burst_size{2000};
};

struct ServerConfig {
    std::string     host{"0.0.0.0"};
    uint16_t        port{8080};
    std::size_t     thread_pool_size{0};
    std::size_t     max_connections{10000};
    std::size_t     backlog{1024};
    std::size_t     max_request_size{1048576};
    int             connection_timeout_ms{30000};
    int             keepalive_timeout_ms{75000};
    int             keepalive_max_requests{1000};
    std::string     log_level{"info"};
    std::string     log_file;
    bool            log_access{true};
    bool            enable_gzip{true};
    int             gzip_level{6};
    std::size_t     gzip_min_size{1024};
    std::string     static_file_directory{"./static"};
    bool            enable_directory_listing{false};
    TlsConfig       tls;
    RateLimitConfig rate_limit;
};

class ConfigLoader {
public:
    ConfigLoader() = default;

    [[nodiscard]] bool              load(const std::string& filepath);
    [[nodiscard]] const ServerConfig& config() const noexcept;
    [[nodiscard]] std::string       error_message() const noexcept;

    static ServerConfig defaults();

private:
    ServerConfig config_;
    std::string  error_message_;
};

}
