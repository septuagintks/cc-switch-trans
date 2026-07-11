#include "transport/header_filter.hpp"

#include <algorithm>
#include <cstddef>
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

bool is_standard_hop_by_hop_name(const std::string& name, bool request) {
    static const std::unordered_set<std::string> skipped = {
        "content-length",
        "connection",
        "keep-alive",
        "proxy-authenticate",
        "proxy-authorization",
        "proxy-connection",
        "te",
        "trailer",
        "transfer-encoding",
        "upgrade",
    };
    return (request && name == "host") || skipped.count(name) != 0;
}

bool connection_nominates(
    const Headers& headers,
    const std::string& normalized_name) {
    for (const auto& [name, value] : headers) {
        if (lower_copy(name) != "connection") {
            continue;
        }
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto end = value.find(',', start);
            auto token_start = start;
            auto token_end = end == std::string::npos ? value.size() : end;
            while (token_start < token_end
                && std::isspace(static_cast<unsigned char>(value[token_start])) != 0) {
                ++token_start;
            }
            while (token_end > token_start
                && std::isspace(static_cast<unsigned char>(value[token_end - 1])) != 0) {
                --token_end;
            }
            if (token_end - token_start == normalized_name.size()
                && std::equal(
                    value.begin() + static_cast<std::ptrdiff_t>(token_start),
                    value.begin() + static_cast<std::ptrdiff_t>(token_end),
                    normalized_name.begin(),
                    [](unsigned char left, unsigned char right) {
                        return std::tolower(left) == std::tolower(right);
                    })) {
                return true;
            }
            if (end == std::string::npos) {
                break;
            }
            start = end + 1;
        }
    }
    return false;
}

Headers filter_headers(const Headers& headers, bool request) {
    Headers filtered;
    filtered.reserve(headers.size());
    for (const auto& [name, value] : headers) {
        const auto normalized_name = lower_copy(name);
        if (!is_standard_hop_by_hop_name(normalized_name, request)
            && !connection_nominates(headers, normalized_name)) {
            filtered.emplace_back(name, value);
        }
    }
    return filtered;
}

} // namespace

Headers filter_request_headers(const Headers& headers) {
    return filter_headers(headers, true);
}

Headers filter_response_headers(const Headers& headers) {
    return filter_headers(headers, false);
}

} // namespace ccs
