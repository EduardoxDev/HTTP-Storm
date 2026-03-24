#pragma once

#include <memory>
#include <string>
#include <string_view>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

namespace hphttp {

class Logger {
public:
    static Logger& instance();

    void init(std::string_view level, std::string_view log_file = "");

    void log_access(std::string_view remote_addr,
                    std::string_view method,
                    std::string_view path,
                    int              status_code,
                    std::size_t      response_size,
                    double           latency_ms);

    std::shared_ptr<spdlog::logger> get() const noexcept;

    template <typename... Args>
    void info(fmt::format_string<Args...> fmt, Args&&... args) {
        logger_->info(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void warn(fmt::format_string<Args...> fmt, Args&&... args) {
        logger_->warn(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void error(fmt::format_string<Args...> fmt, Args&&... args) {
        logger_->error(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void debug(fmt::format_string<Args...> fmt, Args&&... args) {
        logger_->debug(fmt, std::forward<Args>(args)...);
    }

    template <typename... Args>
    void trace(fmt::format_string<Args...> fmt, Args&&... args) {
        logger_->trace(fmt, std::forward<Args>(args)...);
    }

private:
    Logger() = default;

    std::shared_ptr<spdlog::logger> logger_;
    std::shared_ptr<spdlog::logger> access_logger_;
};

}
