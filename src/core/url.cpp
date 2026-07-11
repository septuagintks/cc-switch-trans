#include "core/url.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <stdexcept>
#include <string_view>

namespace ccs {

namespace {

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::uint16_t parse_port(std::string_view text) {
    unsigned int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() || value == 0 || value > 65535) {
        throw std::invalid_argument("invalid upstream port");
    }
    return static_cast<std::uint16_t>(value);
}

bool is_hex(char ch) {
    return (ch >= '0' && ch <= '9')
        || (ch >= 'a' && ch <= 'f')
        || (ch >= 'A' && ch <= 'F');
}

unsigned char hex_value(char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<unsigned char>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<unsigned char>(ch - 'a' + 10);
    }
    return static_cast<unsigned char>(ch - 'A' + 10);
}

char upper_hex(unsigned char value) {
    return value < 10
        ? static_cast<char>('0' + value)
        : static_cast<char>('A' + value - 10);
}

bool is_unreserved(unsigned char ch) {
    return (ch >= 'a' && ch <= 'z')
        || (ch >= 'A' && ch <= 'Z')
        || (ch >= '0' && ch <= '9')
        || ch == '-'
        || ch == '.'
        || ch == '_'
        || ch == '~';
}

} // namespace

ParsedUrl parse_http_url(const std::string& raw_url) {
    const auto scheme_end = raw_url.find("://");
    if (scheme_end == std::string::npos) {
        throw std::invalid_argument("upstream URL must start with http:// or https://");
    }

    ParsedUrl parsed;
    const auto scheme = lower_copy(raw_url.substr(0, scheme_end));
    if (scheme == "http") {
        parsed.port = 80;
    } else if (scheme == "https") {
        parsed.secure = true;
        parsed.port = 443;
    } else {
        throw std::invalid_argument("upstream URL must start with http:// or https://");
    }

    const std::string rest = raw_url.substr(scheme_end + 3);
    const auto slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    parsed.base_path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (authority.empty() || authority.find('@') != std::string::npos) {
        throw std::invalid_argument("invalid upstream authority");
    }

    if (authority.front() == '[') {
        const auto closing = authority.find(']');
        if (closing == std::string::npos) {
            throw std::invalid_argument("invalid IPv6 upstream host");
        }
        parsed.host = authority.substr(1, closing - 1);
        if (closing + 1 < authority.size()) {
            if (authority[closing + 1] != ':') {
                throw std::invalid_argument("invalid upstream authority");
            }
            parsed.port = parse_port(std::string_view(authority).substr(closing + 2));
        }
    } else {
        const auto first_colon = authority.find(':');
        const auto last_colon = authority.rfind(':');
        if (first_colon != std::string::npos && first_colon != last_colon) {
            throw std::invalid_argument("IPv6 upstream hosts must use brackets");
        }
        if (last_colon == std::string::npos) {
            parsed.host = authority;
        } else {
            parsed.host = authority.substr(0, last_colon);
            parsed.port = parse_port(std::string_view(authority).substr(last_colon + 1));
        }
    }

    parsed.host = lower_copy(parsed.host);
    while (!parsed.host.empty() && parsed.host.back() == '.') {
        parsed.host.pop_back();
    }
    if (parsed.host.empty()) {
        throw std::invalid_argument("upstream host is empty");
    }
    if (parsed.base_path.empty()) {
        parsed.base_path = "/";
    }
    return parsed;
}

bool canonicalize_http_path(
    std::string_view raw_path,
    std::string& canonical_path,
    std::string& error) {
    error.clear();
    if (raw_path.empty() || raw_path.front() != '/') {
        error = "path must start with /";
        return false;
    }
    if (raw_path.size() > 2048) {
        error = "path exceeds the maximum length of 2048 bytes";
        return false;
    }

    std::string candidate;
    candidate.reserve(raw_path.size());
    for (std::size_t index = 0; index < raw_path.size(); ++index) {
        const auto ch = static_cast<unsigned char>(raw_path[index]);
        if (ch < 0x20 || ch == 0x7f
            || ch == '?'
            || ch == '#'
            || ch == '\\') {
            error = "path must not contain control characters, query, fragment, or backslash";
            return false;
        }
        if (ch != '%') {
            candidate.push_back(static_cast<char>(ch));
            continue;
        }
        if (index + 2 >= raw_path.size()
            || !is_hex(raw_path[index + 1])
            || !is_hex(raw_path[index + 2])) {
            error = "path contains an invalid percent escape";
            return false;
        }
        const auto decoded = static_cast<unsigned char>(
            (hex_value(raw_path[index + 1]) << 4) | hex_value(raw_path[index + 2]));
        if (decoded < 0x20 || decoded == 0x7f) {
            error = "path must not encode control characters";
            return false;
        }
        if (decoded == '.' || decoded == '/' || decoded == '\\') {
            error = "path must not encode dot or path separators";
            return false;
        }
        if (is_unreserved(decoded)) {
            candidate.push_back(static_cast<char>(decoded));
        } else {
            candidate.push_back('%');
            candidate.push_back(upper_hex(static_cast<unsigned char>(decoded >> 4)));
            candidate.push_back(upper_hex(static_cast<unsigned char>(decoded & 0x0f)));
        }
        index += 2;
    }
    if (candidate.find("//") != std::string::npos) {
        error = "path must not contain duplicate separators";
        return false;
    }
    std::size_t start = 1;
    while (start <= candidate.size()) {
        const auto end = candidate.find('/', start);
        const auto segment = candidate.substr(
            start,
            end == std::string::npos ? std::string::npos : end - start);
        if (segment == "." || segment == "..") {
            error = "path must not contain dot segments";
            return false;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    if (candidate.size() > 1 && candidate.back() == '/') {
        candidate.pop_back();
    }
    canonical_path = std::move(candidate);
    return true;
}

std::string join_url_path(const std::string& base_path, const std::string& route_path, const std::string& query) {
    std::string path = base_path.empty() ? "/" : base_path;
    if (path.back() == '/' && !route_path.empty() && route_path.front() == '/') {
        path.pop_back();
    } else if (path.back() != '/' && (route_path.empty() || route_path.front() != '/')) {
        path.push_back('/');
    }
    path += route_path;
    if (!query.empty()) {
        path += "?";
        path += query;
    }
    return path;
}

} // namespace ccs
