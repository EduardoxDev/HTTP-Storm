#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace hphttp {

enum class HttpMethod {
    GET,
    POST,
    PUT,
    DELETE,
    PATCH,
    HEAD,
    OPTIONS,
    UNKNOWN
};

enum class HttpVersion {
    HTTP_1_0,
    HTTP_1_1,
    HTTP_2_0,
    UNKNOWN
};

enum class ParseState {
    REQUEST_LINE,
    HEADERS,
    BODY,
    COMPLETE,
    ERROR
};

struct HttpRequest {
    HttpMethod                                      method{HttpMethod::UNKNOWN};
    std::string                                     path;
    std::string                                     query_string;
    HttpVersion                                     version{HttpVersion::UNKNOWN};
    std::unordered_map<std::string, std::string>    headers;
    std::unordered_map<std::string, std::string>    query_params;
    std::string                                     body;
    std::string                                     remote_addr;
    bool                                            keep_alive{false};
    std::size_t                                     content_length{0};
};

struct HttpResponse {
    int                                             status_code{200};
    std::string                                     status_message{"OK"};
    std::unordered_map<std::string, std::string>    headers;
    std::string                                     body;
    bool                                            compressed{false};

    void set_header(std::string key, std::string value);
    void set_json_body(std::string json);
    void set_content_type(std::string_view content_type);
    [[nodiscard]] std::string serialize() const;
};

class HttpParser {
public:
    HttpParser() = default;

    enum class FeedResult {
        NEED_MORE_DATA,
        COMPLETE,
        ERROR
    };

    FeedResult feed(std::string_view data);
    void reset();

    [[nodiscard]] const HttpRequest& request() const noexcept;
    [[nodiscard]] bool               is_complete() const noexcept;
    [[nodiscard]] std::string_view   error_message() const noexcept;

    static HttpMethod    parse_method(std::string_view method_str) noexcept;
    static HttpVersion   parse_version(std::string_view version_str) noexcept;
    static std::string   method_to_string(HttpMethod method) noexcept;
    static std::string   url_decode(std::string_view encoded);

private:
    bool parse_request_line(std::string_view line);
    bool parse_header_line(std::string_view line);
    bool parse_query_string(std::string_view query);
    void finalize_headers();

    HttpRequest  request_;
    ParseState   state_{ParseState::REQUEST_LINE};
    std::string  buffer_;
    std::string  error_message_;
    std::size_t  body_bytes_read_{0};
};

}
