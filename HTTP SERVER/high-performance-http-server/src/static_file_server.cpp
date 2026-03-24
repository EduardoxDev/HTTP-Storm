#include "static_file_server.hpp"
#include "logger.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hphttp {

StaticFileServer::StaticFileServer(std::string root_directory, bool enable_directory_listing)
    : root_dir_(std::move(root_directory))
    , enable_directory_listing_(enable_directory_listing)
{
    mime_types_ = {
        {".html",  "text/html; charset=utf-8"},
        {".htm",   "text/html; charset=utf-8"},
        {".css",   "text/css; charset=utf-8"},
        {".js",    "application/javascript; charset=utf-8"},
        {".mjs",   "application/javascript; charset=utf-8"},
        {".json",  "application/json; charset=utf-8"},
        {".xml",   "application/xml; charset=utf-8"},
        {".txt",   "text/plain; charset=utf-8"},
        {".md",    "text/markdown; charset=utf-8"},
        {".png",   "image/png"},
        {".jpg",   "image/jpeg"},
        {".jpeg",  "image/jpeg"},
        {".gif",   "image/gif"},
        {".webp",  "image/webp"},
        {".svg",   "image/svg+xml"},
        {".ico",   "image/x-icon"},
        {".woff",  "font/woff"},
        {".woff2", "font/woff2"},
        {".ttf",   "font/ttf"},
        {".eot",   "application/vnd.ms-fontobject"},
        {".pdf",   "application/pdf"},
        {".zip",   "application/zip"},
        {".gz",    "application/gzip"},
        {".mp4",   "video/mp4"},
        {".webm",  "video/webm"},
        {".mp3",   "audio/mpeg"},
        {".wav",   "audio/wav"},
    };
}

std::optional<HttpResponse> StaticFileServer::serve(const HttpRequest& request) const {
    if (request.method != HttpMethod::GET && request.method != HttpMethod::HEAD) {
        return std::nullopt;
    }

    auto entry = resolve_file(request.path);
    if (!entry) return std::nullopt;

    return serve_file(*entry, request);
}

bool StaticFileServer::can_serve(std::string_view path) const noexcept {
    auto clean = sanitize_path(path);
    if (clean.empty()) return false;

    std::filesystem::path full_path = root_dir_ / clean;
    std::error_code ec;

    if (std::filesystem::is_regular_file(full_path, ec)) return true;
    if (std::filesystem::is_directory(full_path, ec)) {
        auto index_path = full_path / index_file_;
        return std::filesystem::is_regular_file(index_path, ec) || enable_directory_listing_;
    }

    return false;
}

void StaticFileServer::add_custom_mime_type(std::string extension, std::string mime_type) {
    mime_types_[std::move(extension)] = std::move(mime_type);
}

void StaticFileServer::set_cache_max_age(int seconds) {
    cache_max_age_ = seconds;
}

void StaticFileServer::set_index_file(std::string filename) {
    index_file_ = std::move(filename);
}

std::optional<FileEntry> StaticFileServer::resolve_file(std::string_view url_path) const {
    auto clean = sanitize_path(url_path);
    std::filesystem::path full_path = root_dir_ / clean;

    std::error_code ec;
    if (std::filesystem::is_directory(full_path, ec)) {
        full_path = full_path / index_file_;
    }

    if (!std::filesystem::is_regular_file(full_path, ec)) return std::nullopt;

    auto ext      = full_path.extension().string();
    auto size     = std::filesystem::file_size(full_path, ec);
    if (ec) return std::nullopt;

    auto ftime    = std::filesystem::last_write_time(full_path, ec);
    if (ec) return std::nullopt;

    auto sys_time = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ftime - std::filesystem::file_time_type::clock::now() + std::chrono::system_clock::now());

    FileEntry entry;
    entry.path         = full_path.string();
    entry.content_type = get_mime_type(ext);
    entry.size         = size;
    entry.last_modified= sys_time;
    entry.etag         = generate_etag(full_path, size);

    return entry;
}

HttpResponse StaticFileServer::serve_file(const FileEntry& entry, const HttpRequest& request) const {
    if (!is_modified_since(entry, request)) {
        HttpResponse resp;
        resp.status_code    = 304;
        resp.status_message = "Not Modified";
        resp.set_header("ETag", entry.etag);
        return resp;
    }

    std::ifstream file(entry.path, std::ios::binary);
    if (!file.is_open()) {
        HttpResponse resp;
        resp.status_code    = 500;
        resp.status_message = "Internal Server Error";
        resp.set_json_body(R"({"error":"Failed to open file","code":500})");
        return resp;
    }

    HttpResponse resp;
    resp.status_code    = 200;
    resp.status_message = "OK";
    resp.body.assign(std::istreambuf_iterator<char>(file), {});
    resp.set_content_type(entry.content_type);
    resp.set_header("Content-Length",  std::to_string(entry.size));
    resp.set_header("ETag",            entry.etag);
    resp.set_header("Cache-Control",   "public, max-age=" + std::to_string(cache_max_age_));
    resp.set_header("Accept-Ranges",   "bytes");

    auto tt = std::chrono::system_clock::to_time_t(entry.last_modified);
    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&tt), "%a, %d %b %Y %H:%M:%S GMT");
    resp.set_header("Last-Modified", oss.str());

    if (request.method == HttpMethod::HEAD) {
        resp.body.clear();
    }

    return resp;
}

std::string StaticFileServer::get_mime_type(std::string_view extension) const {
    std::string ext_lower(extension);
    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    auto it = mime_types_.find(ext_lower);
    return (it != mime_types_.end()) ? it->second : "application/octet-stream";
}

std::string StaticFileServer::generate_etag(const std::filesystem::path& path, std::size_t size) const {
    std::error_code ec;
    auto ftime = std::filesystem::last_write_time(path, ec);
    if (ec) return "\"unknown\"";

    auto ns = ftime.time_since_epoch().count();
    std::ostringstream oss;
    oss << '"' << std::hex << (ns ^ size) << '"';
    return oss.str();
}

bool StaticFileServer::is_modified_since(const FileEntry& entry, const HttpRequest& request) const {
    auto it = request.headers.find("if-none-match");
    if (it != request.headers.end() && it->second == entry.etag) {
        return false;
    }
    return true;
}

std::string StaticFileServer::sanitize_path(std::string_view url_path) const {
    std::string path(url_path);

    if (!path.empty() && path.front() == '/') {
        path.erase(0, 1);
    }

    std::string result;
    std::istringstream stream(path);
    std::string segment;
    std::vector<std::string> parts;

    while (std::getline(stream, segment, '/')) {
        if (segment == "." || segment.empty()) continue;
        if (segment == "..") {
            if (!parts.empty()) parts.pop_back();
        } else {
            parts.push_back(segment);
        }
    }

    for (const auto& part : parts) {
        if (!result.empty()) result += '/';
        result += part;
    }

    return result;
}

}
