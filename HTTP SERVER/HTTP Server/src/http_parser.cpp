#include "http_parser.hpp"

#include <algorithm>
#include <charconv>
#include <sstream>
#include <stdexcept>

namespace hphttp {

void HttpResponse::set_header(std::string key, std::string value) {
    headers[std::move(key)] = std::move(value);
}

void HttpResponse::set_json_body(std::string json) {
    body = std::move(json);
    headers["Content-Type"] = "application/json; charset=utf-8";
    headers["Content-Length"] = std::to_string(body.size());
}

void HttpResponse::set_content_type(std::string_view content_type) {
    headers["Content-Type"] = std::string(content_type);
}

std::string HttpResponse::serialize() const {
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code << ' ' << status_message << "\r\n";
    for (const auto& [key, value] : headers) {
        oss << key << ": " << value << "\r\n";
    }
    oss << "\r\n";
    oss << body;
    return oss.str();
}

HttpParser::FeedResult HttpParser::feed(std::string_view data) {
    buffer_ += data;

    while (true) {
        if (state_ == ParseState::REQUEST_LINE) {
            auto pos = buffer_.find("\r\n");
            if (pos == std::string::npos) return FeedResult::NEED_MORE_DATA;

            if (!parse_request_line(std::string_view(buffer_.data(), pos))) {
                state_ = ParseState::ERROR;
                return FeedResult::ERROR;
            }
            buffer_.erase(0, pos + 2);
            state_ = ParseState::HEADERS;
        }

        if (state_ == ParseState::HEADERS) {
            while (true) {
                auto pos = buffer_.find("\r\n");
                if (pos == std::string::npos) return FeedResult::NEED_MORE_DATA;

                if (pos == 0) {
                    buffer_.erase(0, 2);
                    finalize_headers();
                    state_ = ParseState::BODY;
                    break;
                }

                if (!parse_header_line(std::string_view(buffer_.data(), pos))) {
                    state_ = ParseState::ERROR;
                    return FeedResult::ERROR;
                }
                buffer_.erase(0, pos + 2);
            }
        }

        if (state_ == ParseState::BODY) {
            if (request_.content_length == 0) {
                state_ = ParseState::COMPLETE;
                return FeedResult::COMPLETE;
            }

            if (buffer_.size() < request_.content_length - body_bytes_read_) {
                request_.body += buffer_;
                body_bytes_read_ += buffer_.size();
                buffer_.clear();
                return FeedResult::NEED_MORE_DATA;
            }

            std::size_t remaining = request_.content_length - body_bytes_read_;
            request_.body += buffer_.substr(0, remaining);
            buffer_.erase(0, remaining);
            state_ = ParseState::COMPLETE;
            return FeedResult::COMPLETE;
        }

        if (state_ == ParseState::COMPLETE) return FeedResult::COMPLETE;
        if (state_ == ParseState::ERROR)    return FeedResult::ERROR;
        break;
    }

    return FeedResult::NEED_MORE_DATA;
}

void HttpParser::reset() {
    request_        = HttpRequest{};
    state_          = ParseState::REQUEST_LINE;
    buffer_.clear();
    error_message_.clear();
    body_bytes_read_ = 0;
}

const HttpRequest& HttpParser::request() const noexcept {
    return request_;
}

bool HttpParser::is_complete() const noexcept {
    return state_ == ParseState::COMPLETE;
}

std::string_view HttpParser::error_message() const noexcept {
    return error_message_;
}

bool HttpParser::parse_request_line(std::string_view line) {
    auto method_end = line.find(' ');
    if (method_end == std::string_view::npos) {
        error_message_ = "Invalid request line: missing method";
        return false;
    }

    request_.method = parse_method(line.substr(0, method_end));
    if (request_.method == HttpMethod::UNKNOWN) {
        error_message_ = "Unknown HTTP method";
        return false;
    }

    auto path_start = method_end + 1;
    auto path_end   = line.find(' ', path_start);
    if (path_end == std::string_view::npos) {
        error_message_ = "Invalid request line: missing version";
        return false;
    }

    std::string_view full_path = line.substr(path_start, path_end - path_start);
    auto query_pos = full_path.find('?');
    if (query_pos != std::string_view::npos) {
        request_.path         = std::string(full_path.substr(0, query_pos));
        request_.query_string = std::string(full_path.substr(query_pos + 1));
        parse_query_string(request_.query_string);
    } else {
        request_.path = std::string(full_path);
    }

    request_.version = parse_version(line.substr(path_end + 1));
    return true;
}

bool HttpParser::parse_header_line(std::string_view line) {
    auto colon_pos = line.find(':');
    if (colon_pos == std::string_view::npos) {
        error_message_ = "Invalid header line";
        return false;
    }

    std::string key(line.substr(0, colon_pos));
    std::string_view value_raw = line.substr(colon_pos + 1);

    auto value_start = value_raw.find_first_not_of(" \t");
    std::string value = (value_start != std::string_view::npos)
                        ? std::string(value_raw.substr(value_start))
                        : std::string{};

    std::transform(key.begin(), key.end(), key.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    request_.headers[std::move(key)] = std::move(value);
    return true;
}

bool HttpParser::parse_query_string(std::string_view query) {
    std::string_view remaining = query;
    while (!remaining.empty()) {
        auto amp_pos = remaining.find('&');
        std::string_view pair = (amp_pos != std::string_view::npos)
                                ? remaining.substr(0, amp_pos)
                                : remaining;

        auto eq_pos = pair.find('=');
        if (eq_pos != std::string_view::npos) {
            std::string key   = url_decode(pair.substr(0, eq_pos));
            std::string value = url_decode(pair.substr(eq_pos + 1));
            request_.query_params[std::move(key)] = std::move(value);
        } else if (!pair.empty()) {
            request_.query_params[std::string(pair)] = "";
        }

        if (amp_pos == std::string_view::npos) break;
        remaining = remaining.substr(amp_pos + 1);
    }
    return true;
}

void HttpParser::finalize_headers() {
    auto it = request_.headers.find("content-length");
    if (it != request_.headers.end()) {
        std::from_chars(it->second.data(),
                        it->second.data() + it->second.size(),
                        request_.content_length);
    }

    auto conn_it = request_.headers.find("connection");
    if (conn_it != request_.headers.end()) {
        std::string conn_val = conn_it->second;
        std::transform(conn_val.begin(), conn_val.end(), conn_val.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        request_.keep_alive = (conn_val.find("keep-alive") != std::string::npos);
    } else {
        request_.keep_alive = (request_.version == HttpVersion::HTTP_1_1);
    }
}

HttpMethod HttpParser::parse_method(std::string_view method_str) noexcept {
    if (method_str == "GET")     return HttpMethod::GET;
    if (method_str == "POST")    return HttpMethod::POST;
    if (method_str == "PUT")     return HttpMethod::PUT;
    if (method_str == "DELETE")  return HttpMethod::DELETE;
    if (method_str == "PATCH")   return HttpMethod::PATCH;
    if (method_str == "HEAD")    return HttpMethod::HEAD;
    if (method_str == "OPTIONS") return HttpMethod::OPTIONS;
    return HttpMethod::UNKNOWN;
}

HttpVersion HttpParser::parse_version(std::string_view version_str) noexcept {
    if (version_str == "HTTP/1.1") return HttpVersion::HTTP_1_1;
    if (version_str == "HTTP/1.0") return HttpVersion::HTTP_1_0;
    if (version_str == "HTTP/2.0") return HttpVersion::HTTP_2_0;
    return HttpVersion::UNKNOWN;
}

std::string HttpParser::method_to_string(HttpMethod method) noexcept {
    switch (method) {
        case HttpMethod::GET:     return "GET";
        case HttpMethod::POST:    return "POST";
        case HttpMethod::PUT:     return "PUT";
        case HttpMethod::DELETE:  return "DELETE";
        case HttpMethod::PATCH:   return "PATCH";
        case HttpMethod::HEAD:    return "HEAD";
        case HttpMethod::OPTIONS: return "OPTIONS";
        default:                  return "UNKNOWN";
    }
}

std::string HttpParser::url_decode(std::string_view encoded) {
    std::string result;
    result.reserve(encoded.size());

    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] == '%' && i + 2 < encoded.size()) {
            int hex_val = 0;
            auto res = std::from_chars(encoded.data() + i + 1,
                                       encoded.data() + i + 3,
                                       hex_val, 16);
            if (res.ec == std::errc{}) {
                result += static_cast<char>(hex_val);
                i += 2;
            } else {
                result += encoded[i];
            }
        } else if (encoded[i] == '+') {
            result += ' ';
        } else {
            result += encoded[i];
        }
    }

    return result;
}

}
