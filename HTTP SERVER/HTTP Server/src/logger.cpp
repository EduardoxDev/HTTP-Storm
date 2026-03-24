#include "logger.hpp"

#include <spdlog/sinks/basic_file_sink.h>
#include <vector>

namespace hphttp {

Logger& Logger::instance() {
    static Logger inst;
    return inst;
}

void Logger::init(std::string_view level, std::string_view log_file) {
    std::vector<spdlog::sink_ptr> sinks;

    auto stdout_sink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    stdout_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%l%$] [%t] %v");
    sinks.push_back(stdout_sink);

    if (!log_file.empty()) {
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            std::string(log_file), 50 * 1024 * 1024, 5);
        file_sink->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%l] [%t] %v");
        sinks.push_back(file_sink);
    }

    logger_ = std::make_shared<spdlog::logger>("hphttp", sinks.begin(), sinks.end());
    logger_->set_level(spdlog::level::from_str(std::string(level)));
    logger_->flush_on(spdlog::level::warn);
    spdlog::register_logger(logger_);
    spdlog::set_default_logger(logger_);

    auto access_stdout = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    access_stdout->set_pattern("%v");

    if (!log_file.empty()) {
        std::string access_file = std::string(log_file) + ".access";
        auto access_file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
            access_file, 100 * 1024 * 1024, 10);
        access_logger_ = std::make_shared<spdlog::logger>(
            "access", std::initializer_list<spdlog::sink_ptr>{access_stdout, access_file_sink});
    } else {
        access_logger_ = std::make_shared<spdlog::logger>("access", access_stdout);
    }

    access_logger_->set_level(spdlog::level::info);
    spdlog::register_logger(access_logger_);
}

void Logger::log_access(std::string_view remote_addr,
                        std::string_view method,
                        std::string_view path,
                        int              status_code,
                        std::size_t      response_size,
                        double           latency_ms) {
    if (!access_logger_) return;
    access_logger_->info("{} {} {} {} {}B {:.2f}ms",
                         remote_addr, method, path,
                         status_code, response_size, latency_ms);
}

std::shared_ptr<spdlog::logger> Logger::get() const noexcept {
    return logger_;
}

}
