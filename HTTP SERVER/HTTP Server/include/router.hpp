#pragma once

#include "http_parser.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <regex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hphttp {

using RouteHandler = std::function<HttpResponse(const HttpRequest&)>;

struct RouteMatch {
    RouteHandler                              handler;
    std::unordered_map<std::string, std::string> params;
};

struct Route {
    HttpMethod   method;
    std::string  pattern;
    std::regex   regex_pattern;
    std::vector<std::string> param_names;
    RouteHandler handler;
    bool         is_parametric{false};
};

class Router {
public:
    Router() = default;

    void get(std::string pattern, RouteHandler handler);
    void post(std::string pattern, RouteHandler handler);
    void put(std::string pattern, RouteHandler handler);
    void del(std::string pattern, RouteHandler handler);
    void patch(std::string pattern, RouteHandler handler);
    void options(std::string pattern, RouteHandler handler);
    void add_route(HttpMethod method, std::string pattern, RouteHandler handler);

    [[nodiscard]] std::optional<RouteMatch> match(HttpMethod method, std::string_view path) const;

    void set_not_found_handler(RouteHandler handler);
    void set_method_not_allowed_handler(RouteHandler handler);

    [[nodiscard]] HttpResponse dispatch(const HttpRequest& request) const;

private:
    static std::pair<std::regex, std::vector<std::string>> compile_pattern(const std::string& pattern);

    std::vector<Route>  routes_;
    RouteHandler        not_found_handler_;
    RouteHandler        method_not_allowed_handler_;
};

}
