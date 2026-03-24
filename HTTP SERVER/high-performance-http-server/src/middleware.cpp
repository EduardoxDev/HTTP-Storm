#include "middleware.hpp"
#include "logger.hpp"

#include <zlib.h>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

namespace hphttp {

void MiddlewareChain::use(MiddlewareFn middleware) {
    middlewares_.push_back(std::move(middleware));
}

HttpResponse MiddlewareChain::execute(const HttpRequest& request, MiddlewareNext final_handler) const {
    if (middlewares_.empty()) {
        return final_handler(request);
    }

    std::function<HttpResponse(std::size_t, const HttpRequest&)> dispatch =
        [&](std::size_t index, const HttpRequest& req) -> HttpResponse {
            if (index >= middlewares_.size()) {
                return final_handler(req);
            }
            return middlewares_[index](req, [&](const HttpRequest& next_req) {
                return dispatch(index + 1, next_req);
            });
        };

    return dispatch(0, request);
}

namespace middleware {

MiddlewareFn request_logger() {
    return [](const HttpRequest& request, MiddlewareNext next) -> HttpResponse {
        auto start = std::chrono::high_resolution_clock::now();
        auto response = next(request);
        auto end = std::chrono::high_resolution_clock::now();

        double latency_ms = std::chrono::duration<double, std::milli>(end - start).count();

        Logger::instance().log_access(
            request.remote_addr,
            HttpParser::method_to_string(request.method),
            request.path,
            response.status_code,
            response.body.size(),
            latency_ms
        );

        return response;
    };
}

MiddlewareFn cors(std::string allowed_origins) {
    return [origins = std::move(allowed_origins)](const HttpRequest& request, MiddlewareNext next) -> HttpResponse {
        if (request.method == HttpMethod::OPTIONS) {
            HttpResponse resp;
            resp.status_code    = 204;
            resp.status_message = "No Content";
            resp.set_header("Access-Control-Allow-Origin", origins);
            resp.set_header("Access-Control-Allow-Methods", "GET, POST, PUT, DELETE, PATCH, OPTIONS");
            resp.set_header("Access-Control-Allow-Headers", "Content-Type, Authorization, X-Request-ID");
            resp.set_header("Access-Control-Max-Age", "86400");
            return resp;
        }

        auto response = next(request);
        response.set_header("Access-Control-Allow-Origin", origins);
        response.set_header("Vary", "Origin");
        return response;
    };
}

MiddlewareFn gzip_compression(int level) {
    return [level](const HttpRequest& request, MiddlewareNext next) -> HttpResponse {
        auto response = next(request);

        if (response.compressed || response.body.empty()) {
            return response;
        }

        auto accept_encoding_it = request.headers.find("accept-encoding");
        if (accept_encoding_it == request.headers.end() ||
            accept_encoding_it->second.find("gzip") == std::string::npos) {
            return response;
        }

        z_stream zs{};
        if (deflateInit2(&zs, level, Z_DEFLATED, 15 | 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return response;
        }

        zs.next_in  = reinterpret_cast<Bytef*>(response.body.data());
        zs.avail_in = static_cast<uInt>(response.body.size());

        std::string compressed;
        compressed.resize(deflateBound(&zs, zs.avail_in));

        zs.next_out  = reinterpret_cast<Bytef*>(compressed.data());
        zs.avail_out = static_cast<uInt>(compressed.size());

        if (deflate(&zs, Z_FINISH) != Z_STREAM_END) {
            deflateEnd(&zs);
            return response;
        }

        compressed.resize(zs.total_out);
        deflateEnd(&zs);

        response.body       = std::move(compressed);
        response.compressed = true;
        response.set_header("Content-Encoding", "gzip");
        response.set_header("Content-Length", std::to_string(response.body.size()));
        response.set_header("Vary", "Accept-Encoding");
        return response;
    };
}

MiddlewareFn rate_limiter(std::size_t requests_per_second, std::size_t burst_size) {
    if (burst_size == 0) burst_size = requests_per_second * 2;

    auto state = std::make_shared<RateLimiterState>();

    return [state, requests_per_second, burst_size](const HttpRequest& request, MiddlewareNext next) -> HttpResponse {
        const std::string& client_ip = request.remote_addr;
        auto now = std::chrono::steady_clock::now();

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            auto& [count, window_start] = state->buckets[client_ip];

            double elapsed = std::chrono::duration<double>(now - window_start).count();
            double replenish = elapsed * static_cast<double>(requests_per_second);
            count = static_cast<std::size_t>(
                std::min(static_cast<double>(burst_size),
                         static_cast<double>(count) + replenish));
            window_start = now;

            if (count == 0) {
                HttpResponse resp;
                resp.status_code    = 429;
                resp.status_message = "Too Many Requests";
                resp.set_header("Retry-After", "1");
                resp.set_header("X-RateLimit-Limit", std::to_string(requests_per_second));
                resp.set_json_body(R"({"error":"Too Many Requests","code":429})");
                return resp;
            }

            --count;
        }

        auto response = next(request);
        response.set_header("X-RateLimit-Limit", std::to_string(requests_per_second));
        return response;
    };
}

MiddlewareFn request_timeout(std::chrono::milliseconds timeout) {
    return [timeout](const HttpRequest& request, MiddlewareNext next) -> HttpResponse {
        auto start = std::chrono::steady_clock::now();
        auto response = next(request);

        auto elapsed = std::chrono::steady_clock::now() - start;
        if (elapsed > timeout) {
            Logger::instance().warn("Request to {} exceeded timeout of {}ms",
                                    request.path,
                                    timeout.count());
        }

        return response;
    };
}

MiddlewareFn security_headers() {
    return [](const HttpRequest& request, MiddlewareNext next) -> HttpResponse {
        auto response = next(request);
        response.set_header("X-Content-Type-Options",     "nosniff");
        response.set_header("X-Frame-Options",            "SAMEORIGIN");
        response.set_header("X-XSS-Protection",          "1; mode=block");
        response.set_header("Referrer-Policy",            "strict-origin-when-cross-origin");
        response.set_header("Permissions-Policy",         "geolocation=(), microphone=(), camera=()");
        response.set_header("Content-Security-Policy",
                            "default-src 'self'; script-src 'self'; style-src 'self' 'unsafe-inline'");
        return response;
    };
}

MiddlewareFn request_id() {
    static std::random_device rd;
    static std::mt19937_64 gen(rd());
    static std::uniform_int_distribution<uint64_t> dist;

    return [](const HttpRequest& request, MiddlewareNext next) -> HttpResponse {
        auto id_it = request.headers.find("x-request-id");
        std::string req_id;

        if (id_it != request.headers.end()) {
            req_id = id_it->second;
        } else {
            std::ostringstream oss;
            oss << std::hex << std::setw(16) << std::setfill('0') << dist(gen);
            req_id = oss.str();
        }

        auto response = next(request);
        response.set_header("X-Request-ID", req_id);
        return response;
    };
}

}
}
