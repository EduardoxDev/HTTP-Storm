#pragma once

#include "http_parser.hpp"
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace hphttp {

struct FileEntry {
    std::string                             path;
    std::string                             content_type;
    std::size_t                             size;
    std::chrono::system_clock::time_point   last_modified;
    std::string                             etag;
};

class StaticFileServer {
public:
    explicit StaticFileServer(std::string root_directory, bool enable_directory_listing = false);

    [[nodiscard]] std::optional<HttpResponse> serve(const HttpRequest& request) const;
    [[nodiscard]] bool                        can_serve(std::string_view path) const noexcept;

    void add_custom_mime_type(std::string extension, std::string mime_type);
    void set_cache_max_age(int seconds);
    void set_index_file(std::string filename);

private:
    [[nodiscard]] std::optional<FileEntry> resolve_file(std::string_view url_path) const;
    [[nodiscard]] HttpResponse             serve_file(const FileEntry& entry, const HttpRequest& request) const;
    [[nodiscard]] HttpResponse             serve_directory(const std::filesystem::path& dir_path,
                                                           std::string_view url_path) const;
    [[nodiscard]] std::string              get_mime_type(std::string_view extension) const;
    [[nodiscard]] std::string              generate_etag(const std::filesystem::path& path,
                                                         std::size_t size) const;
    [[nodiscard]] bool                     is_modified_since(const FileEntry& entry,
                                                             const HttpRequest& request) const;
    [[nodiscard]] std::string              sanitize_path(std::string_view url_path) const;

    std::filesystem::path                           root_dir_;
    bool                                            enable_directory_listing_;
    std::string                                     index_file_{"index.html"};
    int                                             cache_max_age_{3600};
    std::unordered_map<std::string, std::string>    mime_types_;
};

}
