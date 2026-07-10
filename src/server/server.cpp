#include "server/server.hpp"

#include "core/request_id.hpp"
#include "core/transform.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <climits>
#include <condition_variable>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <queue>
#include <sstream>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

namespace ccs {

namespace {

constexpr int kBacklog = 64;
constexpr std::size_t kHeaderLimit = 64 * 1024;

const AppConfig& require_config_snapshot(const ConfigSnapshot& snapshot) {
    if (!snapshot) {
        throw std::invalid_argument("config snapshot must not be null");
    }
    return *snapshot;
}

class RequestMetricsScope {
public:
    RequestMetricsScope(std::shared_ptr<RuntimeMetrics> metrics, CancellationToken cancellation)
        : metrics_(std::move(metrics))
        , cancellation_(std::move(cancellation)) {
        if (metrics_) {
            metrics_->request_started();
        }
    }

    ~RequestMetricsScope() {
        if (metrics_) {
            if (cancellation_.is_cancelled()) {
                metrics_->request_cancelled();
            } else {
                metrics_->request_completed();
            }
        }
    }

private:
    std::shared_ptr<RuntimeMetrics> metrics_;
    CancellationToken cancellation_;
};

class PeriodicReporter {
public:
    PeriodicReporter(int interval_ms, std::function<void()> report)
        : interval_(interval_ms)
        , report_(std::move(report)) {
        if (interval_.count() > 0) {
            thread_ = std::thread([this]() { run(); });
        }
    }

    ~PeriodicReporter() {
        stop();
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!cv_.wait_for(lock, interval_, [this]() { return stopping_; })) {
            lock.unlock();
            report_();
            lock.lock();
        }
    }

    std::chrono::milliseconds interval_;
    std::function<void()> report_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopping_ = false;
    std::thread thread_;
};

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

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

std::string reason_phrase(int status_code) {
    switch (status_code) {
    case 200:
        return "OK";
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 503:
        return "Service Unavailable";
    case 502:
        return "Bad Gateway";
    case 504:
        return "Gateway Timeout";
    case 500:
        return "Internal Server Error";
    default:
        return "Error";
    }
}

std::string error_json(const std::string& message, const std::string& type) {
    return "{\"error\":{\"message\":\"" + message + "\",\"type\":\"" + type + "\"}}";
}

std::string json_escape(const std::string& value) {
    std::ostringstream out;
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\':
            out << "\\\\";
            break;
        case '"':
            out << "\\\"";
            break;
        case '\b':
            out << "\\b";
            break;
        case '\f':
            out << "\\f";
            break;
        case '\n':
            out << "\\n";
            break;
        case '\r':
            out << "\\r";
            break;
        case '\t':
            out << "\\t";
            break;
        default:
            if (ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch);
            } else {
                out << static_cast<char>(ch);
            }
            break;
        }
    }
    return out.str();
}

bool is_sensitive_header(const std::string& name) {
    static const std::unordered_set<std::string> sensitive = {
        "authorization",
        "proxy-authorization",
        "cookie",
        "set-cookie",
        "x-api-key",
    };
    return sensitive.count(lower_copy(name)) != 0;
}

std::string headers_to_json(const Headers& headers, bool redact_sensitive) {
    std::ostringstream out;
    out << "{";
    bool first = true;
    for (const auto& [name, value] : headers) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << "\"" << json_escape(name) << "\":";
        out << "\"" << json_escape(redact_sensitive && is_sensitive_header(name) ? "***" : value) << "\"";
    }
    out << "}";
    return out.str();
}

std::string limited_body(const std::string& body, std::size_t limit) {
    if (body.size() <= limit) {
        return body;
    }
    return body.substr(0, limit);
}

void append_body_fields(std::vector<LogField>& fields, const AppConfig& config, const std::string& body) {
    fields.push_back(field_number("body_size", static_cast<long long>(body.size())));
    if (!config.log_body) {
        return;
    }
    fields.push_back(field_string("body", limited_body(body, config.body_log_limit)));
    fields.push_back(field_bool("body_truncated", body.size() > config.body_log_limit));
}

std::string removed_tools_to_json(const std::vector<RemovedTool>& tools) {
    std::ostringstream out;
    out << "[";
    bool first = true;
    for (const auto& tool : tools) {
        if (!first) {
            out << ",";
        }
        first = false;
        out << "{\"type\":\"" << json_escape(tool.type)
            << "\",\"name\":\"" << json_escape(tool.name) << "\"}";
    }
    out << "]";
    return out.str();
}

std::size_t content_length(const Headers& headers) {
    for (const auto& [name, value] : headers) {
        if (lower_copy(name) == "content-length") {
            try {
                return static_cast<std::size_t>(std::stoull(trim(value)));
            } catch (...) {
                return 0;
            }
        }
    }
    return 0;
}

bool has_json_content_type(const Headers& headers) {
    for (const auto& [name, value] : headers) {
        if (lower_copy(name) == "content-type" && lower_copy(value).find("application/json") != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::size_t leading_json_whitespace(const std::string& body) {
    std::size_t count = 0;
    while (count < body.size()) {
        const char ch = body[count];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        ++count;
    }
    return count;
}

void split_target(HttpRequest& request) {
    const auto pos = request.target.find('?');
    if (pos == std::string::npos) {
        request.path = request.target;
        request.query.clear();
        return;
    }

    request.path = request.target.substr(0, pos);
    request.query = request.target.substr(pos + 1);
}

HttpRequest parse_request(const std::string& raw, const std::string& client_ip) {
    HttpRequest request;
    request.client_ip = client_ip;

    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        throw std::runtime_error("invalid HTTP request");
    }

    std::istringstream stream(raw.substr(0, header_end));
    std::string line;
    if (!std::getline(stream, line)) {
        throw std::runtime_error("missing HTTP request line");
    }
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }

    std::istringstream request_line(line);
    request_line >> request.method >> request.target >> request.version;
    if (request.method.empty() || request.target.empty() || request.version.empty()) {
        throw std::runtime_error("malformed HTTP request line");
    }

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            continue;
        }
        request.headers.emplace_back(trim(line.substr(0, colon)), trim(line.substr(colon + 1)));
    }

    split_target(request);
    request.body = raw.substr(header_end + 4);
    return request;
}

std::string build_response_head(HttpResponse response, bool include_content_length) {
    if (response.reason.empty()) {
        response.reason = reason_phrase(response.status_code);
    }

    bool has_content_type = false;
    for (const auto& [name, _] : response.headers) {
        if (lower_copy(name) == "content-type") {
            has_content_type = true;
            break;
        }
    }
    if (!has_content_type) {
        response.headers.emplace_back("Content-Type", "application/json");
    }
    if (include_content_length) {
        response.headers.emplace_back("Content-Length", std::to_string(response.body.size()));
    }
    response.headers.emplace_back("Connection", "close");

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status_code << " " << response.reason << "\r\n";
    for (const auto& [name, value] : response.headers) {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    return out.str();
}

std::string build_response(HttpResponse response) {
    return build_response_head(response, true) + response.body;
}

std::string build_stream_response_head(HttpResponse response) {
    return build_response_head(std::move(response), false);
}

#ifdef _WIN32

struct ClientJob {
    SOCKET client = INVALID_SOCKET;
    std::string client_ip;
    EndpointGroupKind endpoint = EndpointGroupKind::Responses;
    std::chrono::steady_clock::time_point accepted_at;
};

class ClientCancellationMonitor {
public:
    explicit ClientCancellationMonitor(std::shared_ptr<RuntimeMetrics> metrics)
        : metrics_(std::move(metrics))
        , thread_([this]() { run(); }) {}

    ~ClientCancellationMonitor() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    std::uint64_t watch(SOCKET socket, const CancellationSource& source) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto id = next_id_++;
        entries_.push_back(Entry{id, socket, source});
        cv_.notify_all();
        return id;
    }

    void unwatch(std::uint64_t id) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(), [id](const Entry& entry) {
                return entry.id == id;
            }),
            entries_.end());
    }

    void cancel(const CancellationSource& source) {
        if (source.cancel()) {
            metrics_->client_disconnected();
        }
    }

private:
    struct Entry {
        std::uint64_t id;
        SOCKET socket;
        CancellationSource source;
    };

    void run() {
        while (true) {
            std::vector<Entry> entries;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                cv_.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                    return stopping_ || !entries_.empty();
                });
                if (stopping_) {
                    return;
                }
                entries = entries_;
            }
            if (entries.empty()) {
                continue;
            }

            std::vector<WSAPOLLFD> poll_entries;
            poll_entries.reserve(entries.size());
            for (const auto& entry : entries) {
                poll_entries.push_back(WSAPOLLFD{entry.socket, POLLRDNORM, 0});
            }
            const int result = WSAPoll(poll_entries.data(), static_cast<ULONG>(poll_entries.size()), 50);
            if (result <= 0) {
                continue;
            }

            for (std::size_t i = 0; i < poll_entries.size(); ++i) {
                const auto revents = poll_entries[i].revents;
                bool disconnected = (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0;
                if (!disconnected && (revents & POLLRDNORM) != 0) {
                    char byte = 0;
                    const int received = recv(entries[i].socket, &byte, 1, MSG_PEEK);
                    disconnected = received == 0
                        || (received == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK);
                }
                if (disconnected) {
                    cancel(entries[i].source);
                }
            }
        }
    }

    std::shared_ptr<RuntimeMetrics> metrics_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<Entry> entries_;
    std::uint64_t next_id_ = 1;
    bool stopping_ = false;
    std::thread thread_;
};

class SocketWatch {
public:
    SocketWatch(ClientCancellationMonitor& monitor, SOCKET socket, const CancellationSource& source)
        : monitor_(&monitor)
        , id_(monitor.watch(socket, source)) {}

    ~SocketWatch() {
        stop();
    }

    void stop() {
        if (monitor_ != nullptr) {
            monitor_->unwatch(id_);
            monitor_ = nullptr;
        }
    }

private:
    ClientCancellationMonitor* monitor_;
    std::uint64_t id_;
};

std::atomic_bool& shutdown_requested() {
    static std::atomic_bool value{false};
    return value;
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        shutdown_requested().store(true, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

class WinsockRuntime {
public:
    WinsockRuntime() {
        WSADATA data{};
        const int rc = WSAStartup(MAKEWORD(2, 2), &data);
        if (rc != 0) {
            throw std::runtime_error("WSAStartup failed");
        }
    }

    ~WinsockRuntime() {
        WSACleanup();
    }
};

void close_socket(SOCKET socket) {
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
    }
}

class BoundListener {
public:
    explicit BoundListener(const EndpointGroupConfig& endpoint)
        : endpoint_(&endpoint) {}

    ~BoundListener() {
        close();
    }

    BoundListener(const BoundListener&) = delete;
    BoundListener& operator=(const BoundListener&) = delete;

    bool open(std::string& error) {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* result = nullptr;
        const auto port = std::to_string(endpoint_->listen_port);
        const int rc = getaddrinfo(endpoint_->listen_host.c_str(), port.c_str(), &hints, &result);
        if (rc != 0 || result == nullptr) {
            error = "failed to resolve " + std::string(endpoint_group_name(endpoint_->kind))
                + " listen address: " + endpoint_->listen_host + ":" + port;
            return false;
        }

        for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            socket_ = ::socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (socket_ == INVALID_SOCKET) {
                continue;
            }

            const BOOL exclusive = TRUE;
            if (setsockopt(
                    socket_,
                    SOL_SOCKET,
                    SO_EXCLUSIVEADDRUSE,
                    reinterpret_cast<const char*>(&exclusive),
                    sizeof(exclusive)) == SOCKET_ERROR) {
                close_socket(socket_);
                socket_ = INVALID_SOCKET;
                continue;
            }
            if (bind(socket_, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0) {
                break;
            }
            close_socket(socket_);
            socket_ = INVALID_SOCKET;
        }
        freeaddrinfo(result);

        if (socket_ == INVALID_SOCKET) {
            error = "failed to bind " + std::string(endpoint_group_name(endpoint_->kind))
                + " endpoint " + endpoint_->listen_host + ":" + port;
            return false;
        }
        if (listen(socket_, kBacklog) == SOCKET_ERROR) {
            error = "failed to listen on " + std::string(endpoint_group_name(endpoint_->kind))
                + " endpoint " + endpoint_->listen_host + ":" + port;
            close();
            return false;
        }

        const int accept_timeout_ms = 500;
        setsockopt(
            socket_,
            SOL_SOCKET,
            SO_RCVTIMEO,
            reinterpret_cast<const char*>(&accept_timeout_ms),
            sizeof(accept_timeout_ms));
        return true;
    }

    void close() {
        close_socket(socket_);
        socket_ = INVALID_SOCKET;
    }

    SOCKET socket() const {
        return socket_;
    }

    const EndpointGroupConfig& endpoint() const {
        return *endpoint_;
    }

private:
    const EndpointGroupConfig* endpoint_;
    SOCKET socket_ = INVALID_SOCKET;
};

bool send_all(SOCKET socket, const std::string& data) {
    const char* cursor = data.data();
    std::size_t remaining = data.size();
    while (remaining > 0) {
        const int chunk = remaining > static_cast<std::size_t>(INT_MAX)
            ? INT_MAX
            : static_cast<int>(remaining);
        const int sent = send(socket, cursor, chunk, 0);
        if (sent <= 0) {
            return false;
        }
        cursor += sent;
        remaining -= static_cast<std::size_t>(sent);
    }
    return true;
}

std::string recv_request(SOCKET socket, std::size_t max_body_size) {
    std::string buffer;
    char temp[8192];
    std::size_t header_end = std::string::npos;

    while (header_end == std::string::npos) {
        const int received = recv(socket, temp, sizeof(temp), 0);
        if (received <= 0) {
            if (buffer.empty()) {
                throw std::runtime_error("client disconnected before request");
            }
            throw std::runtime_error("failed to read request");
        }
        buffer.append(temp, static_cast<std::size_t>(received));
        if (buffer.size() > kHeaderLimit + max_body_size) {
            throw std::runtime_error("request too large");
        }
        header_end = buffer.find("\r\n\r\n");
    }

    const HttpRequest partial = parse_request(buffer, "");
    const std::size_t expected_body = content_length(partial.headers);
    if (expected_body > max_body_size) {
        throw std::runtime_error("request body too large");
    }

    const std::size_t body_start = header_end + 4;
    while (buffer.size() < body_start + expected_body) {
        const int received = recv(socket, temp, sizeof(temp), 0);
        if (received <= 0) {
            throw std::runtime_error("failed to read request body");
        }
        buffer.append(temp, static_cast<std::size_t>(received));
    }

    std::size_t actual_body_start = body_start;
    constexpr std::string_view extra_separator = "\r\n\r\n";
    if (expected_body > 0
        && has_json_content_type(partial.headers)
        && buffer.size() >= body_start + extra_separator.size()
        && buffer.compare(body_start, extra_separator.size(), extra_separator) == 0
        && buffer.size() >= body_start + extra_separator.size() + expected_body) {
        actual_body_start += extra_separator.size();
    }

    return buffer.substr(0, body_start) + buffer.substr(actual_body_start, expected_body);
}

std::string peer_ip(sockaddr_storage& storage) {
    char host[NI_MAXHOST]{};
    const auto* addr = reinterpret_cast<sockaddr*>(&storage);
    const int rc = getnameinfo(addr, sizeof(storage), host, sizeof(host), nullptr, 0, NI_NUMERICHOST);
    if (rc != 0) {
        return "";
    }
    return host;
}

void handle_client(
    SOCKET client,
    const Server* server,
    ClientCancellationMonitor& cancellation_monitor,
    std::size_t max_body_size,
    std::string client_ip,
    EndpointGroupKind endpoint) {
    try {
        const auto raw = recv_request(client, max_body_size);
        CancellationSource cancellation_source;
        SocketWatch watch(cancellation_monitor, client, cancellation_source);
        server->process_raw_request_to_sender(
            raw,
            client_ip,
            endpoint,
            [&](const std::string& data) {
                const bool sent = send_all(client, data);
                if (!sent) {
                    cancellation_monitor.cancel(cancellation_source);
                }
                return sent;
            },
            cancellation_source.token());
        watch.stop();
    } catch (const std::exception& ex) {
        if (std::strcmp(ex.what(), "client disconnected before request") == 0) {
            close_socket(client);
            return;
        }
        const bool too_large = std::strcmp(ex.what(), "request body too large") == 0
            || std::strcmp(ex.what(), "request too large") == 0;
        const std::string message = too_large
            ? "request body too large"
            : "internal server error";
        const int status = too_large ? 413 : 500;
        server->log_request_error(
            endpoint, status, too_large ? "invalid_request_error" : "server_error", message);
        HttpResponse response;
        response.status_code = status;
        response.reason = reason_phrase(status);
        response.body = error_json(message, status == 413 ? "invalid_request_error" : "server_error");
        const auto raw_response = build_response(response);
        send_all(client, raw_response);
    }
    close_socket(client);
}

void reject_overloaded_client(SOCKET client, const Server* server, EndpointGroupKind endpoint) {
    server->log_request_error(endpoint, 503, "server_overloaded", "maximum connection count reached");
    HttpResponse response;
    response.status_code = 503;
    response.reason = reason_phrase(503);
    response.body = error_json("server is at connection capacity", "server_overloaded");
    send_all(client, build_response(std::move(response)));
    shutdown(client, SD_SEND);
    const int drain_timeout_ms = 250;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&drain_timeout_ms), sizeof(drain_timeout_ms));
    char drain_buffer[4096];
    while (recv(client, drain_buffer, sizeof(drain_buffer), 0) > 0) {
    }
    close_socket(client);
}

#endif

} // namespace

Server::Server(ConfigSnapshot config)
    : config_snapshot_(std::move(config))
    , config_(require_config_snapshot(config_snapshot_))
    , metrics_(std::make_shared<RuntimeMetrics>())
    , responses_router_({&config_.responses_endpoint})
    , chat_router_({&config_.chat_endpoint})
    , proxy_(config_.timeouts, config_.max_response_body_size, metrics_)
    , logger_(config_, metrics_) {
#ifdef _WIN32
    shutdown_requested().store(false, std::memory_order_relaxed);
#endif
}

void Server::log_request_error(
    EndpointGroupKind endpoint,
    int status_code,
    const std::string& type,
    const std::string& message) const {
    logger_.log("error", "request_error", {
        field_string("request_id", make_request_id()),
        field_string("endpoint", endpoint_group_name(endpoint)),
        field_string("message", message),
        field_string("type", type),
        field_number("status_code", status_code),
    });
}

void Server::request_stop() {
#ifdef _WIN32
    shutdown_requested().store(true, std::memory_order_relaxed);
#endif
}

std::string Server::process_raw_request(
    const std::string& raw,
    const std::string& client_ip,
    EndpointGroupKind endpoint) const {
    std::string output;
    process_raw_request_to_sender(raw, client_ip, endpoint, [&](const std::string& data) {
        output += data;
        return true;
    });
    return output;
}

bool Server::process_raw_request_to_sender(
    const std::string& raw,
    const std::string& client_ip,
    EndpointGroupKind endpoint,
    const std::function<bool(const std::string&)>& sender,
    const CancellationToken& cancellation) const {
    const auto request_id = make_request_id();
    const auto started = std::chrono::steady_clock::now();
    RequestMetricsScope metrics_scope(metrics_, cancellation);
    std::string request_task = "unknown";

    try {
        auto request = parse_request(raw, client_ip);
        const auto& router = endpoint == EndpointGroupKind::Responses ? responses_router_ : chat_router_;
        const auto route = router.route(request.path);
        request_task = route.task == nullptr ? "unknown" : task_name(route.task->kind);
        if (route.task != nullptr && is_usage_task(route.task->kind)) {
            request.request_id = request_id;
            if (!route.configured()) {
                HttpResponse response;
                response.status_code = 404;
                response.reason = reason_phrase(404);
                response.body = error_json("unsupported route", "invalid_request_error");
                logger_.log("info", "usage_rejected", {
                    field_string("request_id", request.request_id),
                    field_string("endpoint", endpoint_group_name(endpoint)),
                    field_string("task", task_name(route.task->kind)),
                    field_number("status_code", response.status_code),
                });
                return sender(build_response(std::move(response)));
            }
            auto response = handle_usage_request(request, *route.endpoint, *route.task, cancellation);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            const auto upstream = route.upstream_target();
            logger_.log("info", "usage_completed", {
                field_string("request_id", request.request_id),
                field_string("endpoint", endpoint_group_name(endpoint)),
                field_string("task", task_name(route.task->kind)),
                field_bool("forwarded", request.method == route.task->method),
                field_string("upstream_url", upstream.base_url),
                field_string("upstream_path", upstream.path),
                field_number("status_code", response.status_code),
                field_number("duration_ms", elapsed.count()),
            });
            return sender(build_response(std::move(response)));
        }

        request.request_id = request_id;
        std::vector<LogField> request_fields = {
            field_string("request_id", request.request_id),
            field_string("api_type", route.task == nullptr ? "unknown" : task_name(route.task->kind)),
            field_string("endpoint", endpoint_group_name(endpoint)),
            field_string("method", request.method),
            field_string("local_path", request.path),
            field_string("query", request.query),
            field_string("client_ip", request.client_ip),
            field_raw("headers", headers_to_json(request.headers, config_.redact_sensitive)),
        };
        append_body_fields(request_fields, config_, request.body);
        logger_.log("info", "request_received", request_fields);

        const bool json_proxy_request = route.task != nullptr
            && (route.task->kind == ApiTaskKind::Responses
                || route.task->kind == ApiTaskKind::ChatCompletions)
            && request.method == "POST"
            && has_json_content_type(request.headers);
        if (json_proxy_request) {
            const auto trim_bytes = leading_json_whitespace(request.body);
            if (trim_bytes > 0 && trim_bytes < request.body.size()) {
                request.body.erase(0, trim_bytes);
                logger_.log("debug", "request_body_normalized", {
                    field_string("request_id", request.request_id),
                    field_string("endpoint", endpoint_group_name(endpoint)),
                    field_number("trimmed_leading_bytes", static_cast<long long>(trim_bytes)),
                    field_number("body_size", static_cast<long long>(request.body.size())),
                });
            }
        }

        TransformResult transform_result;
        transform_result.original_body_size = request.body.size();
        transform_result.rewritten_body_size = request.body.size();
        transform_result.rewrite_reason = "no_transform_configured";
        const bool main_task_ready = route.task != nullptr
            && !is_usage_task(route.task->kind)
            && route.configured()
            && request.method == route.task->method;
        const auto upstream_target = route.upstream_target();
        if (main_task_ready
            && std::find(route.task->transforms.begin(), route.task->transforms.end(), "remove_findcg_image_gen") != route.task->transforms.end()) {
            try {
                transform_result = findcg_transform_.apply(*route.task, upstream_target, request.body);
                if (transform_result.rewritten_body) {
                    request.body = std::move(*transform_result.rewritten_body);
                    transform_result.rewritten_body.reset();
                }
            } catch (const TransformError& ex) {
                logger_.log("error", "request_error", {
                    field_string("request_id", request.request_id),
                    field_string("task", task_name(route.task->kind)),
                    field_string("endpoint", endpoint_group_name(endpoint)),
                    field_string("message", ex.what()),
                    field_string("type", "rewrite_error"),
                    field_number("status_code", ex.status_code()),
                });
                HttpResponse response;
                response.status_code = ex.status_code();
                response.reason = reason_phrase(response.status_code);
                response.body = error_json(ex.what(), ex.response_type());
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);
                std::vector<LogField> sent_fields = {
                    field_string("request_id", request.request_id),
                    field_string("task", task_name(route.task->kind)),
                    field_string("endpoint", endpoint_group_name(endpoint)),
                    field_number("status_code", response.status_code),
                    field_number("duration_ms", elapsed.count()),
                    field_bool("streaming", false),
                };
                append_body_fields(sent_fields, config_, response.body);
                logger_.log("info", "response_sent", sent_fields);
                return sender(build_response(std::move(response)));
            }
        }

        const bool can_proxy_stream = main_task_ready;
        HttpResponse response;
        bool streamed = false;
        std::size_t streamed_body_size = 0;
        std::size_t stream_chunk_count = 0;

        if (can_proxy_stream) {
            std::vector<LogField> upstream_request_fields = {
                field_string("request_id", request.request_id),
                field_string("task", task_name(route.task->kind)),
                field_string("endpoint", endpoint_group_name(route.endpoint->kind)),
                field_string("method", request.method),
                field_string("upstream_url", upstream_target.base_url),
                field_string("upstream_path", upstream_target.path),
                field_string("query", request.query),
                field_bool("rewrite_enabled", transform_result.matched),
                field_string("rewrite_name", transform_result.rewrite_name),
                field_string("rewrite_reason", transform_result.rewrite_reason),
                field_number("removed_tools_count", static_cast<long long>(transform_result.removed_tools.size())),
                field_raw("removed_tools", removed_tools_to_json(transform_result.removed_tools)),
                field_number("original_body_size", static_cast<long long>(transform_result.original_body_size)),
                field_number("rewritten_body_size", static_cast<long long>(transform_result.rewritten_body_size)),
                field_raw("headers", headers_to_json(request.headers, config_.redact_sensitive)),
            };
            append_body_fields(upstream_request_fields, config_, request.body);
            logger_.log("info", "upstream_request", upstream_request_fields);

            try {
                response = proxy_.forward_streaming(
                    request,
                    upstream_target,
                    [&](const HttpResponse& headers_response) {
                        streamed = true;
                        metrics_->stream_started();
                        std::vector<LogField> upstream_response_fields = {
                            field_string("request_id", request.request_id),
                            field_string("task", task_name(route.task->kind)),
                            field_string("endpoint", endpoint_group_name(endpoint)),
                            field_number("status_code", headers_response.status_code),
                            field_bool("streaming", true),
                            field_raw("headers", headers_to_json(headers_response.headers, config_.redact_sensitive)),
                            field_number("body_size", 0),
                        };
                        logger_.log("info", "upstream_response", upstream_response_fields);
                        return sender(build_stream_response_head(headers_response));
                    },
                    [&](const std::string& chunk) {
                        std::vector<LogField> chunk_fields = {
                            field_string("request_id", request.request_id),
                            field_string("task", task_name(route.task->kind)),
                            field_string("endpoint", endpoint_group_name(endpoint)),
                            field_number("chunk_sequence", static_cast<long long>(stream_chunk_count)),
                            field_number("chunk_size", static_cast<long long>(chunk.size())),
                        };
                        append_body_fields(chunk_fields, config_, chunk);
                        logger_.log("info", "stream_chunk", chunk_fields);
                        ++stream_chunk_count;
                        streamed_body_size += chunk.size();
                        const bool sent = sender(chunk);
                        if (sent) {
                            metrics_->stream_chunk_forwarded(chunk.size());
                        }
                        return sent;
                    },
                    cancellation);
            } catch (const ProxyError& ex) {
                if (ex.type() == "client_cancelled") {
                    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - started);
                    logger_.log("info", "request_cancelled", {
                        field_string("request_id", request.request_id),
                        field_string("task", task_name(route.task->kind)),
                        field_string("endpoint", endpoint_group_name(endpoint)),
                        field_number("duration_ms", elapsed.count()),
                    });
                    return false;
                }
                metrics_->upstream_request_failed();
                logger_.log("error", "request_error", {
                    field_string("request_id", request.request_id),
                    field_string("task", task_name(route.task->kind)),
                    field_string("endpoint", endpoint_group_name(endpoint)),
                    field_string("message", ex.what()),
                    field_string("type", ex.type()),
                    field_number("status_code", ex.status_code()),
                });
                if (streamed) {
                    return true;
                }
                response.status_code = ex.status_code();
                response.reason = reason_phrase(response.status_code);
                response.body = error_json(ex.what(), ex.type());
            }

            if (!streamed) {
                if (response.reason.empty()) {
                    response.reason = reason_phrase(response.status_code);
                }
                std::vector<LogField> upstream_response_fields = {
                    field_string("request_id", request.request_id),
                    field_string("task", task_name(route.task->kind)),
                    field_string("endpoint", endpoint_group_name(endpoint)),
                    field_number("status_code", response.status_code),
                    field_bool("streaming", false),
                    field_raw("headers", headers_to_json(response.headers, config_.redact_sensitive)),
                };
                append_body_fields(upstream_response_fields, config_, response.body);
                logger_.log("info", "upstream_response", upstream_response_fields);
            }
        } else {
            response = handle_local_route_error(request, route);
            if (response.status_code >= 400) {
                const std::string error_message = response.status_code == 404
                    ? "unsupported route"
                    : response.status_code == 405 ? "method not allowed" : "task upstream is not configured";
                const std::string error_type = response.status_code == 500
                    ? "configuration_error"
                    : "invalid_request_error";
                logger_.log("error", "request_error", {
                    field_string("request_id", request.request_id),
                    field_string("task", route.task == nullptr ? "unknown" : task_name(route.task->kind)),
                    field_string("endpoint", endpoint_group_name(endpoint)),
                    field_string("message", error_message),
                    field_string("type", error_type),
                    field_number("status_code", response.status_code),
                });
            }
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        std::vector<LogField> sent_fields = {
            field_string("request_id", request.request_id),
            field_string("task", route.task == nullptr ? "unknown" : task_name(route.task->kind)),
            field_string("endpoint", endpoint_group_name(endpoint)),
            field_number("status_code", response.status_code),
            field_number("duration_ms", elapsed.count()),
            field_bool("streaming", streamed),
            field_number("stream_chunk_count", static_cast<long long>(stream_chunk_count)),
            field_number("streamed_body_size", static_cast<long long>(streamed_body_size)),
            field_raw("headers", headers_to_json(response.headers, config_.redact_sensitive)),
        };
        if (!streamed) {
            append_body_fields(sent_fields, config_, response.body);
        }
        logger_.log("info", "response_sent", sent_fields);

        if (streamed) {
            return true;
        }
        return sender(build_response(std::move(response)));
    } catch (const ProxyError& ex) {
        if (ex.type() == "client_cancelled") {
            logger_.log("info", "request_cancelled", {
                field_string("request_id", request_id),
                field_string("task", request_task),
                field_string("endpoint", endpoint_group_name(endpoint)),
            });
            return false;
        }
        throw;
    } catch (const std::exception& ex) {
        logger_.log("error", "request_error", {
            field_string("request_id", request_id),
            field_string("endpoint", endpoint_group_name(endpoint)),
            field_string("message", ex.what()),
            field_string("type", "server_error"),
            field_number("status_code", 500),
        });

        HttpResponse response;
        response.status_code = 500;
        response.reason = reason_phrase(500);
        response.body = error_json("internal server error", "server_error");
        return sender(build_response(response));
    }
}

void Server::log_performance_snapshot(const std::string& reason) const {
    const auto snapshot = metrics_->snapshot();
    logger_.log("info", "performance_snapshot", {
        field_string("reason", reason),
        field_number("connections_accepted", static_cast<long long>(snapshot.connections_accepted)),
        field_number("connections_rejected", static_cast<long long>(snapshot.connections_rejected)),
        field_number("connections_completed", static_cast<long long>(snapshot.connections_completed)),
        field_number("current_connections", static_cast<long long>(snapshot.current_connections)),
        field_number("peak_connections", static_cast<long long>(snapshot.peak_connections)),
        field_number("current_queued_connections", static_cast<long long>(snapshot.current_queued_connections)),
        field_number("peak_queued_connections", static_cast<long long>(snapshot.peak_queued_connections)),
        field_number("current_active_workers", static_cast<long long>(snapshot.current_active_workers)),
        field_number("peak_active_workers", static_cast<long long>(snapshot.peak_active_workers)),
        field_number("connection_queue_wait_time_us", static_cast<long long>(snapshot.connection_queue_wait_time_us)),
        field_number("max_connection_queue_wait_us", static_cast<long long>(snapshot.max_connection_queue_wait_us)),
        field_number("responses_connections_accepted", static_cast<long long>(snapshot.responses_endpoint.connections_accepted)),
        field_number("responses_connections_rejected", static_cast<long long>(snapshot.responses_endpoint.connections_rejected)),
        field_number("responses_connections_completed", static_cast<long long>(snapshot.responses_endpoint.connections_completed)),
        field_number("responses_current_connections", static_cast<long long>(snapshot.responses_endpoint.current_connections)),
        field_number("responses_peak_connections", static_cast<long long>(snapshot.responses_endpoint.peak_connections)),
        field_number("responses_current_queued_connections", static_cast<long long>(snapshot.responses_endpoint.current_queued_connections)),
        field_number("responses_peak_queued_connections", static_cast<long long>(snapshot.responses_endpoint.peak_queued_connections)),
        field_number("responses_current_active_workers", static_cast<long long>(snapshot.responses_endpoint.current_active_workers)),
        field_number("responses_peak_active_workers", static_cast<long long>(snapshot.responses_endpoint.peak_active_workers)),
        field_number("responses_connection_queue_wait_time_us", static_cast<long long>(snapshot.responses_endpoint.connection_queue_wait_time_us)),
        field_number("responses_max_connection_queue_wait_us", static_cast<long long>(snapshot.responses_endpoint.max_connection_queue_wait_us)),
        field_number("chat_connections_accepted", static_cast<long long>(snapshot.chat_endpoint.connections_accepted)),
        field_number("chat_connections_rejected", static_cast<long long>(snapshot.chat_endpoint.connections_rejected)),
        field_number("chat_connections_completed", static_cast<long long>(snapshot.chat_endpoint.connections_completed)),
        field_number("chat_current_connections", static_cast<long long>(snapshot.chat_endpoint.current_connections)),
        field_number("chat_peak_connections", static_cast<long long>(snapshot.chat_endpoint.peak_connections)),
        field_number("chat_current_queued_connections", static_cast<long long>(snapshot.chat_endpoint.current_queued_connections)),
        field_number("chat_peak_queued_connections", static_cast<long long>(snapshot.chat_endpoint.peak_queued_connections)),
        field_number("chat_current_active_workers", static_cast<long long>(snapshot.chat_endpoint.current_active_workers)),
        field_number("chat_peak_active_workers", static_cast<long long>(snapshot.chat_endpoint.peak_active_workers)),
        field_number("chat_connection_queue_wait_time_us", static_cast<long long>(snapshot.chat_endpoint.connection_queue_wait_time_us)),
        field_number("chat_max_connection_queue_wait_us", static_cast<long long>(snapshot.chat_endpoint.max_connection_queue_wait_us)),
        field_number("requests_started", static_cast<long long>(snapshot.requests_started)),
        field_number("requests_completed", static_cast<long long>(snapshot.requests_completed)),
        field_number("requests_cancelled", static_cast<long long>(snapshot.requests_cancelled)),
        field_number("client_disconnects", static_cast<long long>(snapshot.client_disconnects)),
        field_number("sse_streams_started", static_cast<long long>(snapshot.sse_streams_started)),
        field_number("stream_chunks_forwarded", static_cast<long long>(snapshot.stream_chunks_forwarded)),
        field_number("stream_bytes_forwarded", static_cast<long long>(snapshot.stream_bytes_forwarded)),
        field_number("upstream_requests_started", static_cast<long long>(snapshot.upstream_requests_started)),
        field_number("upstream_requests_completed", static_cast<long long>(snapshot.upstream_requests_completed)),
        field_number("upstream_requests_cancelled", static_cast<long long>(snapshot.upstream_requests_cancelled)),
        field_number("upstream_requests_failed", static_cast<long long>(snapshot.upstream_requests_failed)),
        field_number("upstream_resolve_timeouts", static_cast<long long>(snapshot.upstream_resolve_timeouts)),
        field_number("upstream_connect_timeouts", static_cast<long long>(snapshot.upstream_connect_timeouts)),
        field_number("upstream_send_timeouts", static_cast<long long>(snapshot.upstream_send_timeouts)),
        field_number("upstream_response_header_timeouts", static_cast<long long>(snapshot.upstream_response_header_timeouts)),
        field_number("upstream_stream_idle_timeouts", static_cast<long long>(snapshot.upstream_stream_idle_timeouts)),
        field_number("upstream_response_body_timeouts", static_cast<long long>(snapshot.upstream_response_body_timeouts)),
        field_number("upstream_total_timeouts", static_cast<long long>(snapshot.upstream_total_timeouts)),
        field_number("upstream_connection_handles_created", static_cast<long long>(snapshot.upstream_connection_handles_created)),
        field_number("upstream_request_handles_created", static_cast<long long>(snapshot.upstream_request_handles_created)),
        field_number("upstream_bytes_sent", static_cast<long long>(snapshot.upstream_bytes_sent)),
        field_number("upstream_bytes_received", static_cast<long long>(snapshot.upstream_bytes_received)),
        field_number("winhttp_connecting_events", static_cast<long long>(snapshot.winhttp_connecting_events)),
        field_number("winhttp_connected_events", static_cast<long long>(snapshot.winhttp_connected_events)),
        field_number("winhttp_connection_closed_events", static_cast<long long>(snapshot.winhttp_connection_closed_events)),
        field_number("log_records_enqueued", static_cast<long long>(snapshot.log_records_enqueued)),
        field_number("log_records_written", static_cast<long long>(snapshot.log_records_written)),
        field_number("log_bytes_written", static_cast<long long>(snapshot.log_bytes_written)),
        field_number("current_log_queue_records", static_cast<long long>(snapshot.current_log_queue_records)),
        field_number("peak_log_queue_records", static_cast<long long>(snapshot.peak_log_queue_records)),
        field_number("current_log_queue_bytes", static_cast<long long>(snapshot.current_log_queue_bytes)),
        field_number("peak_log_queue_bytes", static_cast<long long>(snapshot.peak_log_queue_bytes)),
        field_number("log_backpressure_count", static_cast<long long>(snapshot.log_backpressure_count)),
        field_number("log_backpressure_wait_us", static_cast<long long>(snapshot.log_backpressure_wait_us)),
        field_number("log_batches_written", static_cast<long long>(snapshot.log_batches_written)),
        field_number("log_flush_count", static_cast<long long>(snapshot.log_flush_count)),
        field_number("log_write_time_us", static_cast<long long>(snapshot.log_write_time_us)),
        field_number("log_batch_wait_time_us", static_cast<long long>(snapshot.log_batch_wait_time_us)),
        field_number("max_log_batch_wait_us", static_cast<long long>(snapshot.max_log_batch_wait_us)),
        field_number("log_file_write_time_us", static_cast<long long>(snapshot.log_file_write_time_us)),
        field_number("max_log_file_write_time_us", static_cast<long long>(snapshot.max_log_file_write_time_us)),
        field_number("log_file_flush_time_us", static_cast<long long>(snapshot.log_file_flush_time_us)),
        field_number("max_log_file_flush_time_us", static_cast<long long>(snapshot.max_log_file_flush_time_us)),
        field_number("oldest_log_record_age_us", static_cast<long long>(snapshot.oldest_log_record_age_us)),
        field_number("max_log_record_age_us", static_cast<long long>(snapshot.max_log_record_age_us)),
        field_number("log_writer_failures", static_cast<long long>(snapshot.log_writer_failures)),
        field_bool("log_writer_healthy", snapshot.log_writer_healthy != 0),
        field_number("max_log_batch_records", static_cast<long long>(snapshot.max_log_batch_records)),
        field_number("max_log_batch_bytes", static_cast<long long>(snapshot.max_log_batch_bytes)),
    });
}

HttpResponse Server::handle_usage_request(
    const HttpRequest& request,
    const EndpointGroupConfig& endpoint,
    const TaskConfig& task,
    const CancellationToken& cancellation) const {
    if (request.method != "GET") {
        HttpResponse response;
        response.status_code = 405;
        response.reason = reason_phrase(405);
        response.headers.emplace_back("Allow", "GET");
        response.body = error_json("method not allowed", "invalid_request_error");
        return response;
    }

    try {
        auto response = proxy_.forward(request, endpoint.upstream_target(task), cancellation);
        if (response.reason.empty()) {
            response.reason = reason_phrase(response.status_code);
        }
        return response;
    } catch (const ProxyError& ex) {
        if (ex.type() == "client_cancelled") {
            throw;
        }
        metrics_->upstream_request_failed();
        HttpResponse response;
        response.status_code = ex.status_code();
        response.reason = reason_phrase(response.status_code);
        response.body = error_json(ex.what(), ex.type());
        return response;
    }
}

HttpResponse Server::handle_local_route_error(const HttpRequest& request, const RouteDecision& route) const {
    if (route.task == nullptr) {
        HttpResponse response;
        response.status_code = 404;
        response.reason = reason_phrase(404);
        response.body = error_json("unsupported route", "invalid_request_error");
        return response;
    }

    const auto& task = *route.task;
    if (!route.configured()) {
        HttpResponse response;
        response.status_code = 500;
        response.reason = reason_phrase(500);
        response.body = error_json("task upstream is not configured", "configuration_error");
        return response;
    }

    if (request.method != task.method) {
        HttpResponse response;
        response.status_code = 405;
        response.reason = reason_phrase(405);
        response.headers.emplace_back("Allow", task.method);
        response.body = error_json("method not allowed", "invalid_request_error");
        return response;
    }

    HttpResponse response;
    response.status_code = 500;
    response.reason = reason_phrase(500);
    response.body = error_json("request was not dispatched", "server_error");
    return response;
}

int Server::run(const StartupCallback& startup_callback) {
    bool startup_reported = false;
    const auto report_startup = [&](bool succeeded, const std::string& error) {
        if (startup_reported) {
            return;
        }
        startup_reported = true;
        if (startup_callback) {
            startup_callback(succeeded, error);
        }
    };
#ifndef _WIN32
    const std::string error = "ccs-trans currently supports the built-in HTTP server on Windows only";
    report_startup(false, error);
    std::cerr << error << "\n";
    return 1;
#else
    try {
        WinsockRuntime winsock;
        std::string log_error;
        if (!logger_.open(log_error)) {
            report_startup(false, log_error);
            std::cerr << log_error << "\n";
            return 1;
        }
        SetConsoleCtrlHandler(console_handler, TRUE);

        BoundListener responses_listener(config_.responses_endpoint);
        BoundListener chat_listener(config_.chat_endpoint);
        std::string listener_error;
        if (!responses_listener.open(listener_error) || !chat_listener.open(listener_error)) {
            report_startup(false, listener_error);
            std::cerr << listener_error << "\n";
            SetConsoleCtrlHandler(console_handler, FALSE);
            return 1;
        }

        std::cout << "ccs-trans listening on http://"
                  << config_.responses_endpoint.listen_host << ":" << config_.responses_endpoint.listen_port
                  << " (responses) and http://"
                  << config_.chat_endpoint.listen_host << ":" << config_.chat_endpoint.listen_port
                  << " (chat)\n";
        logger_.log("info", "server_start", {
            field_number("listener_count", 2),
            field_string("responses_listen_host", config_.responses_endpoint.listen_host),
            field_number("responses_listen_port", config_.responses_endpoint.listen_port),
            field_bool("responses_enabled", config_.responses_endpoint.enabled()),
            field_string("responses_upstream_url", config_.responses_endpoint.upstream_url),
            field_string("responses_upstream_path", config_.responses_endpoint.main_task.upstream_path),
            field_string("responses_usage_upstream_path", config_.responses_endpoint.usage_task.upstream_path),
            field_string("chat_listen_host", config_.chat_endpoint.listen_host),
            field_number("chat_listen_port", config_.chat_endpoint.listen_port),
            field_bool("chat_enabled", config_.chat_endpoint.enabled()),
            field_string("chat_upstream_url", config_.chat_endpoint.upstream_url),
            field_string("chat_upstream_path", config_.chat_endpoint.main_task.upstream_path),
            field_string("chat_usage_upstream_path", config_.chat_endpoint.usage_task.upstream_path),
            field_string("log_path", config_.log_path.string()),
            field_number("worker_threads", static_cast<long long>(config_.worker_threads)),
            field_number("max_connections", static_cast<long long>(config_.max_connections)),
            field_number("metrics_interval_ms", config_.metrics_interval_ms),
            field_number("resolve_timeout_ms", config_.timeouts.resolve_ms),
            field_number("connect_timeout_ms", config_.timeouts.connect_ms),
            field_number("send_timeout_ms", config_.timeouts.send_ms),
            field_number("response_header_timeout_ms", config_.timeouts.response_header_ms),
            field_number("stream_idle_timeout_ms", config_.timeouts.stream_idle_ms),
            field_number("total_timeout_ms", config_.timeouts.total_ms),
        });

        PeriodicReporter reporter(config_.metrics_interval_ms, [this]() {
            log_performance_snapshot("periodic");
        });

        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::queue<ClientJob> client_queue;
        std::size_t open_connections = 0;
        bool accepting_done = false;
        ClientCancellationMonitor cancellation_monitor(metrics_);

        std::vector<std::thread> workers;
        workers.reserve(config_.worker_threads);
        try {
            for (std::size_t i = 0; i < config_.worker_threads; ++i) {
                workers.emplace_back([&]() {
                    while (true) {
                        ClientJob job;
                        {
                            std::unique_lock<std::mutex> lock(queue_mutex);
                            queue_cv.wait(lock, [&]() {
                                return accepting_done || !client_queue.empty();
                            });
                            if (client_queue.empty()) {
                                return;
                            }
                            job = std::move(client_queue.front());
                            client_queue.pop();
                            const auto queue_wait = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - job.accepted_at);
                            metrics_->worker_started(
                                job.endpoint,
                                client_queue.size(),
                                static_cast<std::uint64_t>(queue_wait.count()));
                        }
                        handle_client(
                            job.client,
                            this,
                            cancellation_monitor,
                            config_.max_request_body_size,
                            std::move(job.client_ip),
                            job.endpoint);
                        {
                            std::lock_guard<std::mutex> lock(queue_mutex);
                            --open_connections;
                            metrics_->worker_finished(job.endpoint, open_connections);
                        }
                    }
                });
            }
        } catch (...) {
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                accepting_done = true;
            }
            queue_cv.notify_all();
            for (auto& worker : workers) {
                worker.join();
            }
            throw;
        }

        const auto accept_loop = [&](const BoundListener& listener) {
            while (!shutdown_requested().load(std::memory_order_relaxed)) {
                sockaddr_storage client_addr{};
                int client_addr_len = sizeof(client_addr);
                SOCKET client = accept(
                    listener.socket(),
                    reinterpret_cast<sockaddr*>(&client_addr),
                    &client_addr_len);
                if (client == INVALID_SOCKET) {
                    const int error = WSAGetLastError();
                    if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                        continue;
                    }
                    if (shutdown_requested().load(std::memory_order_relaxed)) {
                        return;
                    }
                    log_request_error(
                        listener.endpoint().kind,
                        500,
                        "listener_error",
                        "listener accept failed: " + std::to_string(error));
                    shutdown_requested().store(true, std::memory_order_relaxed);
                    return;
                }
                bool queued = false;
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    if (open_connections < config_.max_connections) {
                        ++open_connections;
                        client_queue.push(ClientJob{
                            client,
                            peer_ip(client_addr),
                            listener.endpoint().kind,
                            std::chrono::steady_clock::now(),
                        });
                        metrics_->connection_accepted(
                            listener.endpoint().kind, open_connections, client_queue.size());
                        queued = true;
                    }
                }
                if (!queued) {
                    metrics_->connection_rejected(listener.endpoint().kind);
                    reject_overloaded_client(client, this, listener.endpoint().kind);
                    continue;
                }
                queue_cv.notify_one();
            }
        };

        std::vector<std::thread> acceptors;
        acceptors.reserve(2);
        try {
            acceptors.emplace_back(accept_loop, std::cref(responses_listener));
            acceptors.emplace_back(accept_loop, std::cref(chat_listener));
        } catch (...) {
            shutdown_requested().store(true, std::memory_order_relaxed);
            for (auto& acceptor : acceptors) {
                acceptor.join();
            }
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                accepting_done = true;
            }
            queue_cv.notify_all();
            for (auto& worker : workers) {
                worker.join();
            }
            throw;
        }

        report_startup(true, {});

        for (auto& acceptor : acceptors) {
            acceptor.join();
        }
        responses_listener.close();
        chat_listener.close();
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            accepting_done = true;
        }
        queue_cv.notify_all();
        for (auto& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        reporter.stop();
        log_performance_snapshot("server_stop");
        logger_.log("info", "server_stop", {
            field_number("listener_count", 2),
        });
        SetConsoleCtrlHandler(console_handler, FALSE);
        return 0;
    } catch (const std::exception& ex) {
        report_startup(false, ex.what());
        std::cerr << "server failed: " << ex.what() << "\n";
        SetConsoleCtrlHandler(console_handler, FALSE);
        return 1;
    } catch (...) {
        const std::string error = "server failed with an unknown exception";
        report_startup(false, error);
        std::cerr << error << "\n";
        SetConsoleCtrlHandler(console_handler, FALSE);
        return 1;
    }
#endif
}

} // namespace ccs
