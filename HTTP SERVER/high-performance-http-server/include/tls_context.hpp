#pragma once

#include "config_loader.hpp"
#include <memory>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdexcept>
#include <string>

namespace hphttp {

struct SslDeleter {
    void operator()(SSL_CTX* ctx) const noexcept { if (ctx) SSL_CTX_free(ctx); }
    void operator()(SSL* ssl)     const noexcept { if (ssl) SSL_free(ssl); }
};

using SslCtxPtr = std::unique_ptr<SSL_CTX, SslDeleter>;
using SslPtr    = std::unique_ptr<SSL, SslDeleter>;

class TlsContext {
public:
    explicit TlsContext(const TlsConfig& config);
    ~TlsContext() = default;

    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    [[nodiscard]] SSL_CTX* native_handle() const noexcept;
    [[nodiscard]] SslPtr   create_ssl(int fd) const;
    [[nodiscard]] bool     is_valid() const noexcept;

    static std::string last_error_string();

private:
    void configure_context(const TlsConfig& config);
    void set_cipher_list(const std::string& ciphers);
    void load_certificates(const TlsConfig& config);

    SslCtxPtr ctx_;
};

}
