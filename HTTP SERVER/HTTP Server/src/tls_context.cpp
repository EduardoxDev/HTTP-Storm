#include "tls_context.hpp"
#include "logger.hpp"

#include <openssl/ssl.h>
#include <openssl/err.h>

namespace hphttp {

TlsContext::TlsContext(const TlsConfig& config) {
    SSL_load_error_strings();
    OpenSSL_add_ssl_algorithms();

    SSL_CTX* raw_ctx = SSL_CTX_new(TLS_server_method());
    if (!raw_ctx) {
        throw std::runtime_error("Failed to create SSL context: " + last_error_string());
    }

    ctx_.reset(raw_ctx);
    configure_context(config);
}

SSL_CTX* TlsContext::native_handle() const noexcept {
    return ctx_.get();
}

SslPtr TlsContext::create_ssl(int fd) const {
    SSL* ssl = SSL_new(ctx_.get());
    if (!ssl) {
        throw std::runtime_error("Failed to create SSL object: " + last_error_string());
    }
    SSL_set_fd(ssl, fd);
    return SslPtr(ssl);
}

bool TlsContext::is_valid() const noexcept {
    return ctx_ != nullptr;
}

std::string TlsContext::last_error_string() {
    char buf[256];
    ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
    return std::string(buf);
}

void TlsContext::configure_context(const TlsConfig& config) {
    SSL_CTX_set_options(ctx_.get(),
                        SSL_OP_NO_SSLv2 |
                        SSL_OP_NO_SSLv3 |
                        SSL_OP_NO_TLSv1 |
                        SSL_OP_NO_TLSv1_1 |
                        SSL_OP_CIPHER_SERVER_PREFERENCE |
                        SSL_OP_SINGLE_DH_USE |
                        SSL_OP_SINGLE_ECDH_USE);

    SSL_CTX_set_mode(ctx_.get(), SSL_MODE_AUTO_RETRY | SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    SSL_CTX_set_min_proto_version(ctx_.get(), TLS1_2_VERSION);

    set_cipher_list(config.ciphers);
    load_certificates(config);

    if (config.verify_client) {
        SSL_CTX_set_verify(ctx_.get(),
                           SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                           nullptr);
    }

    SSL_CTX_set_session_cache_mode(ctx_.get(), SSL_SESS_CACHE_SERVER);
    SSL_CTX_sess_set_cache_size(ctx_.get(), 1024);
}

void TlsContext::set_cipher_list(const std::string& ciphers) {
    if (SSL_CTX_set_cipher_list(ctx_.get(), ciphers.c_str()) != 1) {
        Logger::instance().warn("Failed to set cipher list: {}", last_error_string());
    }
    if (SSL_CTX_set_ciphersuites(ctx_.get(),
                                  "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256") != 1) {
        Logger::instance().warn("Failed to set TLS 1.3 ciphersuites");
    }
}

void TlsContext::load_certificates(const TlsConfig& config) {
    if (!config.cert_file.empty()) {
        if (SSL_CTX_use_certificate_chain_file(ctx_.get(), config.cert_file.c_str()) != 1) {
            throw std::runtime_error("Failed to load certificate: " + last_error_string());
        }
    }

    if (!config.key_file.empty()) {
        if (SSL_CTX_use_PrivateKey_file(ctx_.get(), config.key_file.c_str(), SSL_FILETYPE_PEM) != 1) {
            throw std::runtime_error("Failed to load private key: " + last_error_string());
        }

        if (SSL_CTX_check_private_key(ctx_.get()) != 1) {
            throw std::runtime_error("Private key does not match certificate");
        }
    }

    if (!config.ca_file.empty()) {
        if (SSL_CTX_load_verify_locations(ctx_.get(), config.ca_file.c_str(), nullptr) != 1) {
            throw std::runtime_error("Failed to load CA file: " + last_error_string());
        }
    }
}

}
