#include "header_filter.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <unordered_set>

namespace ccs {

namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool is_request_hop_header(const std::string& name) {
    static const std::unordered_set<std::string> skipped = {
        "host",
        "content-length",
        "connection",
        "transfer-encoding",
    };
    return skipped.count(lower_copy(name)) != 0;
}

bool is_response_hop_header(const std::string& name) {
    static const std::unordered_set<std::string> skipped = {
        "content-length",
        "connection",
        "transfer-encoding",
    };
    return skipped.count(lower_copy(name)) != 0;
}

} // namespace

Headers filter_request_headers(const Headers& headers) {
    Headers filtered;
    for (const auto& [name, value] : headers) {
        if (!is_request_hop_header(name)) {
            filtered.emplace_back(name, value);
        }
    }
    return filtered;
}

Headers filter_response_headers(const Headers& headers) {
    Headers filtered;
    for (const auto& [name, value] : headers) {
        if (!is_response_hop_header(name)) {
            filtered.emplace_back(name, value);
        }
    }
    return filtered;
}

} // namespace ccs
