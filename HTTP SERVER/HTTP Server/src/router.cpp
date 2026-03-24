#include "router.hpp"

#include <stdexcept>
#include <sstream>

namespace hphttp {

void Router::get(std::string pattern, RouteHandler handler) {
    add_route(HttpMethod::GET, std::move(pattern), std::move(handler));
}

void Router::post(std::string pattern, RouteHandler handler) {
    add_route(HttpMethod::POST, std::move(pattern), std::move(handler));
}

void Router::put(std::string pattern, RouteHandler handler) {
    add_route(HttpMethod::PUT, std::move(pattern), std::move(handler));
}

void Router::del(std::string pattern, RouteHandler handler) {
    add_route(HttpMethod::DELETE, std::move(pattern), std::move(handler));
}

void Router::patch(std::string pattern, RouteHandler handler) {
    add_route(HttpMethod::PATCH, std::move(pattern), std::move(handler));
}

void Router::options(std::string pattern, RouteHandler handler) {
    add_route(HttpMethod::OPTIONS, std::move(pattern), std::move(handler));
}

void Router::add_route(HttpMethod method, std::string pattern, RouteHandler handler) {
    auto [regex_pat, param_names] = compile_pattern(pattern);

    Route route;
    route.method        = method;
    route.pattern       = pattern;
    route.regex_pattern = std::move(regex_pat);
    route.param_names   = std::move(param_names);
    route.handler       = std::move(handler);
    route.is_parametric = !route.param_names.empty() || pattern.find('*') != std::string::npos;

    routes_.push_back(std::move(route));
}

std::optional<RouteMatch> Router::match(HttpMethod method, std::string_view path) const {
    bool method_matched = false;

    for (const auto& route : routes_) {
        std::string path_str(path);
        std::smatch match;

        if (!std::regex_match(path_str, match, route.regex_pattern)) continue;

        if (route.method != method) {
            method_matched = true;
            continue;
        }

        RouteMatch result;
        result.handler = route.handler;

        for (std::size_t i = 0; i < route.param_names.size(); ++i) {
            if (i + 1 < match.size()) {
                result.params[route.param_names[i]] = match[i + 1].str();
            }
        }

        return result;
    }

    if (method_matched && method_not_allowed_handler_) {
        return RouteMatch{method_not_allowed_handler_, {}};
    }

    return std::nullopt;
}

void Router::set_not_found_handler(RouteHandler handler) {
    not_found_handler_ = std::move(handler);
}

void Router::set_method_not_allowed_handler(RouteHandler handler) {
    method_not_allowed_handler_ = std::move(handler);
}

HttpResponse Router::dispatch(const HttpRequest& request) const {
    auto result = match(request.method, request.path);

    if (result) {
        return result->handler(request);
    }

    if (not_found_handler_) {
        return not_found_handler_(request);
    }

    HttpResponse resp;
    resp.status_code    = 404;
    resp.status_message = "Not Found";
    resp.set_json_body(R"({"error":"Not Found","code":404})");
    return resp;
}

std::pair<std::regex, std::vector<std::string>> Router::compile_pattern(const std::string& pattern) {
    std::string regex_str;
    std::vector<std::string> param_names;

    regex_str.reserve(pattern.size() * 2 + 4);
    regex_str += '^';

    for (std::size_t i = 0; i < pattern.size(); ++i) {
        char c = pattern[i];

        if (c == ':') {
            std::size_t start = i + 1;
            while (i + 1 < pattern.size() && pattern[i + 1] != '/' && pattern[i + 1] != '.') {
                ++i;
            }
            std::string param_name = pattern.substr(start, i - start + 1);
            param_names.push_back(param_name);
            regex_str += "([^/]+)";
        } else if (c == '*') {
            regex_str += "(.*)";
        } else if (c == '.') {
            regex_str += "\\.";
        } else if (c == '?') {
            regex_str += "\\?";
        } else {
            regex_str += c;
        }
    }

    regex_str += '$';

    return {std::regex(regex_str, std::regex::optimize), std::move(param_names)};
}

}
