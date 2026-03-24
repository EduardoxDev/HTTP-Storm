#pragma once

#include "http_parser.hpp"
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include <atomic>
#include <mutex>

namespace hphttp {

using MiddlewareNext = std::function<HttpResponse(const HttpRequest&)>;
using MiddlewareFn   = std::function<HttpResponse(const HttpRequest&, MiddlewareNext)>;

class MiddlewareChain {
public:
    MiddlewareChain() = default;

    void use(MiddlewareFn middleware);
    [[nodiscard]] HttpResponse execute(const HttpRequest& request, MiddlewareNext final_handler) const;

private:
    std::vector<MiddlewareFn> middlewares_;
};

namespace middleware {

MiddlewareFn request_logger();
MiddlewareFn cors(std::string allowed_origins = "*");
MiddlewareFn gzip_compression(int level = 6);
MiddlewareFn rate_limiter(std::size_t requests_per_second, std::size_t burst_size = 0);
MiddlewareFn request_timeout(std::chrono::milliseconds timeout);
MiddlewareFn security_headers();
MiddlewareFn request_id();

struct RateLimiterState {
    std::unordered_map<std::string, std::pair<std::size_t, std::chrono::steady_clock::time_point>> buckets;
    std::mutex mutex;
};

}

}
