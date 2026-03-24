#include "config_loader.hpp"

#include <fstream>
#include <nlohmann/json.hpp>
#include <thread>

namespace hphttp {

bool ConfigLoader::load(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        error_message_ = "Cannot open config file: " + filepath;
        return false;
    }

    nlohmann::json j;
    try {
        file >> j;
    } catch (const nlohmann::json::parse_error& e) {
        error_message_ = std::string("JSON parse error: ") + e.what();
        return false;
    }

    try {
        if (j.contains("host"))                   config_.host                  = j["host"].get<std::string>();
        if (j.contains("port"))                   config_.port                  = j["port"].get<uint16_t>();
        if (j.contains("thread_pool_size"))       config_.thread_pool_size      = j["thread_pool_size"].get<std::size_t>();
        if (j.contains("max_connections"))        config_.max_connections       = j["max_connections"].get<std::size_t>();
        if (j.contains("backlog"))                config_.backlog               = j["backlog"].get<std::size_t>();
        if (j.contains("max_request_size"))       config_.max_request_size      = j["max_request_size"].get<std::size_t>();
        if (j.contains("connection_timeout_ms"))  config_.connection_timeout_ms = j["connection_timeout_ms"].get<int>();
        if (j.contains("keepalive_timeout_ms"))   config_.keepalive_timeout_ms  = j["keepalive_timeout_ms"].get<int>();
        if (j.contains("keepalive_max_requests")) config_.keepalive_max_requests= j["keepalive_max_requests"].get<int>();
        if (j.contains("log_level"))              config_.log_level             = j["log_level"].get<std::string>();
        if (j.contains("log_file"))               config_.log_file              = j["log_file"].get<std::string>();
        if (j.contains("log_access"))             config_.log_access            = j["log_access"].get<bool>();
        if (j.contains("enable_gzip"))            config_.enable_gzip           = j["enable_gzip"].get<bool>();
        if (j.contains("gzip_level"))             config_.gzip_level            = j["gzip_level"].get<int>();
        if (j.contains("gzip_min_size"))          config_.gzip_min_size         = j["gzip_min_size"].get<std::size_t>();
        if (j.contains("static_file_directory"))  config_.static_file_directory = j["static_file_directory"].get<std::string>();
        if (j.contains("enable_directory_listing")) config_.enable_directory_listing = j["enable_directory_listing"].get<bool>();

        if (j.contains("tls")) {
            const auto& tls = j["tls"];
            if (tls.contains("enabled"))       config_.tls.enabled       = tls["enabled"].get<bool>();
            if (tls.contains("cert_file"))     config_.tls.cert_file     = tls["cert_file"].get<std::string>();
            if (tls.contains("key_file"))      config_.tls.key_file      = tls["key_file"].get<std::string>();
            if (tls.contains("ca_file"))       config_.tls.ca_file       = tls["ca_file"].get<std::string>();
            if (tls.contains("ciphers"))       config_.tls.ciphers       = tls["ciphers"].get<std::string>();
            if (tls.contains("verify_client")) config_.tls.verify_client = tls["verify_client"].get<bool>();
        }

        if (j.contains("rate_limit")) {
            const auto& rl = j["rate_limit"];
            if (rl.contains("enabled"))             config_.rate_limit.enabled             = rl["enabled"].get<bool>();
            if (rl.contains("requests_per_second")) config_.rate_limit.requests_per_second = rl["requests_per_second"].get<std::size_t>();
            if (rl.contains("burst_size"))          config_.rate_limit.burst_size          = rl["burst_size"].get<std::size_t>();
        }

    } catch (const nlohmann::json::exception& e) {
        error_message_ = std::string("Config value error: ") + e.what();
        return false;
    }

    if (config_.thread_pool_size == 0) {
        config_.thread_pool_size = std::max(1u, std::thread::hardware_concurrency());
    }

    return true;
}

const ServerConfig& ConfigLoader::config() const noexcept {
    return config_;
}

std::string ConfigLoader::error_message() const noexcept {
    return error_message_;
}

ServerConfig ConfigLoader::defaults() {
    ServerConfig cfg;
    cfg.thread_pool_size = std::max(1u, std::thread::hardware_concurrency());
    return cfg;
}

}
