#pragma once

#include <cstdint>
#include <string>

namespace ccs {

struct ParsedUrl {
    bool secure = false;
    std::string host;
    std::uint16_t port = 0;
    std::string base_path = "/";
};

ParsedUrl parse_http_url(const std::string& raw_url);
std::string join_url_path(const std::string& base_path, const std::string& route_path, const std::string& query);
bool is_findcg_host(const std::string& host);

} // namespace ccs
