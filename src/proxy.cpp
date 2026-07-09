#include "proxy.hpp"

#include "header_filter.hpp"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#endif

namespace ccs {

namespace {

struct ParsedUrl {
    bool secure = false;
    std::string host;
    INTERNET_PORT port = 0;
    std::string base_path = "/";
};

std::wstring widen(const std::string& value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int required = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return std::wstring(value.begin(), value.end());
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required);
    return result;
#else
    return std::wstring(value.begin(), value.end());
#endif
}

std::string narrow(const std::wstring& value) {
#ifdef _WIN32
    if (value.empty()) {
        return {};
    }
    const int required = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return std::string(value.begin(), value.end());
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), required, nullptr, nullptr);
    return result;
#else
    return std::string(value.begin(), value.end());
#endif
}

std::string trim(std::string value) {
    auto is_space = [](unsigned char ch) { return std::isspace(ch) != 0; };
    value.erase(value.begin(), std::find_if(value.begin(), value.end(), [&](char ch) {
                    return !is_space(static_cast<unsigned char>(ch));
                }));
    value.erase(std::find_if(value.rbegin(), value.rend(), [&](char ch) {
                    return !is_space(static_cast<unsigned char>(ch));
                }).base(),
        value.end());
    return value;
}

ParsedUrl parse_url(const std::string& raw_url) {
    ParsedUrl parsed;
    std::string rest;
    if (raw_url.rfind("http://", 0) == 0) {
        parsed.secure = false;
        parsed.port = INTERNET_DEFAULT_HTTP_PORT;
        rest = raw_url.substr(7);
    } else if (raw_url.rfind("https://", 0) == 0) {
        parsed.secure = true;
        parsed.port = INTERNET_DEFAULT_HTTPS_PORT;
        rest = raw_url.substr(8);
    } else {
        throw ProxyError(502, "upstream_error", "upstream URL must start with http:// or https://");
    }

    const auto slash = rest.find('/');
    const std::string authority = slash == std::string::npos ? rest : rest.substr(0, slash);
    parsed.base_path = slash == std::string::npos ? "/" : rest.substr(slash);
    if (parsed.base_path.empty()) {
        parsed.base_path = "/";
    }

    const auto colon = authority.rfind(':');
    if (colon == std::string::npos) {
        parsed.host = authority;
    } else {
        parsed.host = authority.substr(0, colon);
        const auto port_text = authority.substr(colon + 1);
        try {
            parsed.port = static_cast<INTERNET_PORT>(std::stoul(port_text));
        } catch (...) {
            throw ProxyError(502, "upstream_error", "invalid upstream port");
        }
    }

    if (parsed.host.empty()) {
        throw ProxyError(502, "upstream_error", "upstream host is empty");
    }

    return parsed;
}

std::string join_path(const std::string& base_path, const std::string& route_path, const std::string& query) {
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

std::wstring build_header_block(const Headers& headers) {
    std::ostringstream out;
    for (const auto& [name, value] : filter_request_headers(headers)) {
        out << name << ": " << value << "\r\n";
    }
    return widen(out.str());
}

Headers parse_raw_headers(const std::wstring& raw_headers) {
    Headers headers;
    std::istringstream stream(narrow(raw_headers));
    std::string line;
    bool first = true;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }
        if (first) {
            first = false;
            continue;
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }
    return filter_response_headers(headers);
}

std::string winhttp_error_message(const std::string& prefix) {
#ifdef _WIN32
    const auto err = GetLastError();
    std::ostringstream out;
    out << prefix << " (winhttp error " << err << ")";
    return out.str();
#else
    return prefix;
#endif
}

#ifdef _WIN32

class WinHttpHandle {
public:
    WinHttpHandle() = default;
    explicit WinHttpHandle(HINTERNET handle)
        : handle_(handle) {}

    WinHttpHandle(const WinHttpHandle&) = delete;
    WinHttpHandle& operator=(const WinHttpHandle&) = delete;

    WinHttpHandle(WinHttpHandle&& other) noexcept
        : handle_(other.handle_) {
        other.handle_ = nullptr;
    }

    WinHttpHandle& operator=(WinHttpHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    ~WinHttpHandle() {
        reset();
    }

    HINTERNET get() const {
        return handle_;
    }

    explicit operator bool() const {
        return handle_ != nullptr;
    }

private:
    void reset() {
        if (handle_) {
            WinHttpCloseHandle(handle_);
            handle_ = nullptr;
        }
    }

    HINTERNET handle_ = nullptr;
};

void apply_timeouts(HINTERNET handle, int timeout_ms) {
    WinHttpSetTimeouts(handle, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
}

std::wstring query_raw_headers(HINTERNET request) {
    DWORD size = 0;
    WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || size == 0) {
        return {};
    }
    std::wstring buffer(size / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, buffer.data(), &size, WINHTTP_NO_HEADER_INDEX)) {
        return {};
    }
    if (!buffer.empty() && buffer.back() == L'\0') {
        buffer.pop_back();
    }
    return buffer;
}

int query_status_code(HINTERNET request) {
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to query upstream status"));
    }
    return static_cast<int>(status);
}

std::string read_body(HINTERNET request) {
    std::string body;
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) {
            throw ProxyError(502, "upstream_error", winhttp_error_message("failed to query upstream response data"));
        }
        if (available == 0) {
            break;
        }

        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request, chunk.data(), available, &read)) {
            throw ProxyError(502, "upstream_error", winhttp_error_message("failed to read upstream response data"));
        }
        chunk.resize(read);
        body += chunk;
    }
    return body;
}

#endif

} // namespace

ProxyError::ProxyError(int status_code, std::string type, std::string message)
    : std::runtime_error(message)
    , status_code_(status_code)
    , type_(std::move(type)) {}

int ProxyError::status_code() const {
    return status_code_;
}

const std::string& ProxyError::type() const {
    return type_;
}

Proxy::Proxy(AppConfig config)
    : config_(std::move(config)) {}

HttpResponse Proxy::forward(const HttpRequest& request, const std::string& upstream_path) const {
#ifndef _WIN32
    (void)request;
    (void)upstream_path;
    throw ProxyError(502, "upstream_error", "upstream forwarding is implemented with WinHTTP on Windows");
#else
    const auto upstream = parse_url(config_.upstream_url);
    const auto path = join_path(upstream.base_path, upstream_path, request.query);

    WinHttpHandle session(WinHttpOpen(L"ccs-trans/0.1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to open WinHTTP session"));
    }
    apply_timeouts(session.get(), config_.timeout_ms);

    WinHttpHandle connection(WinHttpConnect(session.get(), widen(upstream.host).c_str(), upstream.port, 0));
    if (!connection) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to connect to upstream"));
    }

    const DWORD flags = upstream.secure ? WINHTTP_FLAG_SECURE : 0;
    WinHttpHandle upstream_request(WinHttpOpenRequest(
        connection.get(),
        widen(request.method).c_str(),
        widen(path).c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        flags));
    if (!upstream_request) {
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to create upstream request"));
    }
    apply_timeouts(upstream_request.get(), config_.timeout_ms);

    const auto header_block = build_header_block(request.headers);
    const void* body_data = request.body.empty() ? WINHTTP_NO_REQUEST_DATA : request.body.data();
    const DWORD body_size = static_cast<DWORD>(request.body.size());
    if (!WinHttpSendRequest(
            upstream_request.get(),
            header_block.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_block.c_str(),
            header_block.empty() ? 0 : static_cast<DWORD>(-1L),
            const_cast<void*>(body_data),
            body_size,
            body_size,
            0)) {
        const auto err = GetLastError();
        if (err == ERROR_WINHTTP_TIMEOUT) {
            throw ProxyError(504, "upstream_timeout", "upstream request timed out");
        }
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to send upstream request"));
    }

    if (!WinHttpReceiveResponse(upstream_request.get(), nullptr)) {
        const auto err = GetLastError();
        if (err == ERROR_WINHTTP_TIMEOUT) {
            throw ProxyError(504, "upstream_timeout", "upstream request timed out");
        }
        throw ProxyError(502, "upstream_error", winhttp_error_message("failed to receive upstream response"));
    }

    HttpResponse response;
    response.status_code = query_status_code(upstream_request.get());
    response.headers = parse_raw_headers(query_raw_headers(upstream_request.get()));
    response.body = read_body(upstream_request.get());
    return response;
#endif
}

} // namespace ccs
