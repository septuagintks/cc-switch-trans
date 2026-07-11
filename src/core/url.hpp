#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace ccs {

struct ParsedUrl {
    bool secure = false;
    std::string host;
    std::uint16_t port = 0;
    std::string base_path = "/";
};

ParsedUrl parse_http_url(const std::string& raw_url);
bool canonicalize_http_path(
    std::string_view raw_path,
    std::string& canonical_path,
    std::string& error);
std::string join_url_path(const std::string& base_path, const std::string& route_path, const std::string& query);

} // namespace ccs
