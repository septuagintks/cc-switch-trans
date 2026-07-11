#include "server/server.hpp"

#include "core/request_id.hpp"

#include <nlohmann/json.hpp>

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

const RuntimeSnapshot& require_runtime_snapshot(const RuntimeSnapshotPtr& snapshot) {
    if (!snapshot) {
        throw std::invalid_argument("runtime snapshot must not be null");
    }
    return *snapshot;
}

std::uint64_t next_generation_id() {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

LoggerConfig make_logger_config(const RuntimeSnapshot& snapshot) {
    return LoggerConfig{
        snapshot.log_path,
        snapshot.application.logging.level,
        static_cast<std::size_t>(snapshot.application.logging.queue_capacity),
        static_cast<int>(snapshot.application.logging.flush_interval_ms),
    };
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
    return nlohmann::json({
        {"error", {
            {"message", message},
            {"type", type},
        }},
    }).dump();
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

void append_body_fields(
    std::vector<LogField>& fields,
    const LoggingSettings& logging,
    const std::string& body) {
    fields.push_back(field_number("body_size", static_cast<long long>(body.size())));
    if (!logging.body) {
        return;
    }
    const auto limit = static_cast<std::size_t>(logging.body_limit);
    fields.push_back(field_string("body", limited_body(body, limit)));
    fields.push_back(field_bool("body_truncated", body.size() > limit));
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

std::atomic_bool& console_shutdown_requested() {
    static std::atomic_bool value{false};
    return value;
}

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT || event == CTRL_CLOSE_EVENT) {
        console_shutdown_requested().store(true, std::memory_order_relaxed);
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
    explicit BoundListener(const ListenerSettings& settings)
        : settings_(&settings) {}

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
        const auto port = std::to_string(settings_->port);
        const int rc = getaddrinfo(settings_->host.c_str(), port.c_str(), &hints, &result);
        if (rc != 0 || result == nullptr) {
            error = "failed to resolve listen address: " + settings_->host + ":" + port;
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
            error = "failed to bind listener " + settings_->host + ":" + port;
            return false;
        }
        if (listen(socket_, kBacklog) == SOCKET_ERROR) {
            error = "failed to listen on " + settings_->host + ":" + port;
            close();
            return false;
        }

        return true;
    }

    void close() {
        close_socket(socket_);
        socket_ = INVALID_SOCKET;
    }

    SOCKET socket() const {
        return socket_;
    }

private:
    const ListenerSettings* settings_;
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

template <typename ProcessRequest>
void handle_client(
    SOCKET client,
    const Server* server,
    ClientCancellationMonitor& cancellation_monitor,
    std::size_t max_body_size,
    std::string client_ip,
    ProcessRequest&& process_request) {
    try {
        const auto raw = recv_request(client, max_body_size);
        CancellationSource cancellation_source;
        SocketWatch watch(cancellation_monitor, client, cancellation_source);
        process_request(
            raw,
            client_ip,
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
            status, too_large ? "invalid_request_error" : "server_error", message);
        HttpResponse response;
        response.status_code = status;
        response.reason = reason_phrase(status);
        response.body = error_json(message, status == 413 ? "invalid_request_error" : "server_error");
        const auto raw_response = build_response(response);
        send_all(client, raw_response);
    }
    close_socket(client);
}

void reject_overloaded_client(SOCKET client, const Server* server) {
    try {
        server->log_request_error(503, "server_overloaded", "maximum connection count reached");
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
    } catch (...) {
    }
    close_socket(client);
}

#endif

} // namespace

class Server::RequestGeneration {
public:
    RequestGeneration(
        std::uint64_t generation_id,
        RuntimeSnapshotPtr runtime_snapshot,
        const std::shared_ptr<RuntimeMetrics>& metrics,
        std::shared_ptr<Logger> shared_logger)
        : id(generation_id)
        , snapshot(std::move(runtime_snapshot))
        , runtime(require_runtime_snapshot(snapshot))
        , transport(make_upstream_transport(
              runtime.application.timeouts,
              static_cast<std::size_t>(runtime.application.runtime.max_response_body_size),
              metrics))
        , logger(std::move(shared_logger)) {
        if (!logger) {
            throw std::invalid_argument("request generation must have a logger");
        }
    }

    std::uint64_t id;
    RuntimeSnapshotPtr snapshot;
    const RuntimeSnapshot& runtime;
    std::unique_ptr<UpstreamTransport> transport;
    std::shared_ptr<Logger> logger;
};

Server::Server(RuntimeSnapshotPtr snapshot, LogSinkFactory log_sink_factory)
    : metrics_(std::make_shared<RuntimeMetrics>())
    , log_sink_factory_(std::move(log_sink_factory)) {
    const auto& runtime = require_runtime_snapshot(snapshot);
    generation_ = std::make_shared<RequestGeneration>(
        next_generation_id(), std::move(snapshot), metrics_, make_logger(runtime));
}

Server::~Server() {
    request_stop();
    std::unique_lock<std::shared_mutex> lock(generation_mutex_);
    generation_.reset();
}

std::shared_ptr<Logger> Server::make_logger(const RuntimeSnapshot& snapshot) {
    auto sink = log_sink_factory_ ? log_sink_factory_() : nullptr;
    return std::make_shared<Logger>(
        make_logger_config(snapshot),
        metrics_,
        std::move(sink),
        [this](const std::string& error) { handle_logger_failure(error); });
}

void Server::handle_logger_failure(const std::string& error) noexcept {
    bool expected = false;
    const bool first_failure = fatal_error_.compare_exchange_strong(
        expected, true, std::memory_order_acq_rel);
    if (first_failure) {
        std::cerr << "log writer failed: " << error << "\n";
    }
    request_stop();
}

std::shared_ptr<Server::RequestGeneration> Server::current_generation() const {
    std::shared_lock<std::shared_mutex> lock(generation_mutex_);
    return generation_;
}

void Server::log_request_error(
    int status_code,
    const std::string& type,
    const std::string& message) const {
    const auto generation = current_generation();
    generation->logger->log("error", "request_error", {
        field_string("request_id", make_request_id()),
        field_number("generation_id", static_cast<long long>(generation->id)),
        field_string("message", message),
        field_string("type", type),
        field_number("status_code", status_code),
    });
}

void Server::request_stop() {
    stop_requested_.store(true, std::memory_order_release);
}

ReloadResult Server::reload(RuntimeSnapshotPtr snapshot, std::string& error) {
    error.clear();
    if (!snapshot) {
        error = "runtime snapshot must not be null";
        return ReloadResult::Failed;
    }
    if (!snapshot->protocols || !snapshot->rules || snapshot->routes.empty()) {
        error = "runtime snapshot is incomplete";
        return ReloadResult::Failed;
    }

    std::lock_guard<std::mutex> lock(reload_mutex_);
    const auto current = current_generation();
    const auto& old_runtime = current->runtime;
    const auto& old_application = old_runtime.application;
    const auto& new_application = snapshot->application;
    const bool listener_changed = old_application.listener.host != new_application.listener.host
        || old_application.listener.port != new_application.listener.port;
    const bool execution_changed =
        old_application.runtime.worker_threads != new_application.runtime.worker_threads
        || old_application.runtime.metrics_interval_ms != new_application.runtime.metrics_interval_ms;
    const bool same_log_path = old_runtime.log_path == snapshot->log_path;
    const bool same_log_writer_settings =
        old_application.logging.level == new_application.logging.level
        && old_application.logging.queue_capacity == new_application.logging.queue_capacity
        && old_application.logging.flush_interval_ms == new_application.logging.flush_interval_ms;
    if (listener_changed || execution_changed || (same_log_path && !same_log_writer_settings)) {
        current->logger->log("info", "config_reload", {
            field_number("generation_id", static_cast<long long>(current->id)),
            field_string("mode", "graceful_restart_required"),
            field_bool("listener_changed", listener_changed),
            field_bool("execution_changed", execution_changed),
            field_bool("log_writer_changed", same_log_path && !same_log_writer_settings),
        });
        error = "configuration changes require a graceful service restart";
        return ReloadResult::RestartRequired;
    }

    std::shared_ptr<Logger> logger = current->logger;
    if (!same_log_path) {
        try {
            logger = make_logger(*snapshot);
            std::string log_error;
            if (!logger->open(log_error)) {
                error = log_error;
                return ReloadResult::Failed;
            }
        } catch (const std::exception& ex) {
            error = "failed to create candidate log writer: " + std::string(ex.what());
            return ReloadResult::Failed;
        } catch (...) {
            error = "failed to create candidate log writer";
            return ReloadResult::Failed;
        }
    }

    try {
        auto next = std::make_shared<RequestGeneration>(
            next_generation_id(), std::move(snapshot), metrics_, std::move(logger));
        {
            std::unique_lock<std::shared_mutex> generation_lock(generation_mutex_);
            generation_ = next;
        }
        next->logger->log("info", "config_reload", {
            field_number("generation_id", static_cast<long long>(next->id)),
            field_number("previous_generation_id", static_cast<long long>(current->id)),
            field_string("mode", "generation_swap"),
            field_number("profile_count", static_cast<long long>(next->runtime.profiles.size())),
            field_number("route_count", static_cast<long long>(next->runtime.routes.size())),
            field_string("log_path", next->runtime.log_path.string()),
        });
    } catch (const std::exception& ex) {
        error = "failed to build config generation: " + std::string(ex.what());
        return ReloadResult::Failed;
    }
    return ReloadResult::Applied;
}

std::string Server::process_raw_request(
    const std::string& raw,
    const std::string& client_ip) const {
    std::string output;
    process_raw_request_to_sender(raw, client_ip, [&](const std::string& data) {
        output += data;
        return true;
    });
    return output;
}

bool Server::process_raw_request_to_sender(
    const std::string& raw,
    const std::string& client_ip,
    const std::function<bool(const std::string&)>& sender,
    const CancellationToken& cancellation) const {
    return process_with_generation(
        current_generation(), raw, client_ip, sender, cancellation);
}

bool Server::process_with_generation(
    const std::shared_ptr<RequestGeneration>& generation,
    const std::string& raw,
    const std::string& client_ip,
    const std::function<bool(const std::string&)>& sender,
    const CancellationToken& cancellation) const {
    const auto& runtime = generation->runtime;
    const auto& logging = runtime.application.logging;
    const auto request_id = make_request_id();
    const auto started = std::chrono::steady_clock::now();
    RequestMetricsScope metrics_scope(metrics_, cancellation);
    std::string profile_id = "unknown";
    std::string protocol_id = "unknown";
    std::string route_kind = "unknown";
    std::shared_ptr<const ProtocolHandler> protocol_handler;

    try {
        auto request = parse_request(raw, client_ip);
        request.request_id = request_id;
        const auto lookup = runtime.routes.lookup(request.method, request.path);
        if (lookup.status != RouteLookupStatus::Matched || lookup.entry == nullptr) {
            auto response = handle_local_route_error(lookup);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            generation->logger->log("error", "request_error", {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("method", request.method),
                field_string("local_path", request.path),
                field_string("message", lookup.status == RouteLookupStatus::MethodNotAllowed
                        ? "method not allowed"
                        : lookup.status == RouteLookupStatus::InvalidPath
                            ? "invalid request path"
                            : "unsupported route"),
                field_string("type", "invalid_request_error"),
                field_number("status_code", response.status_code),
                field_number("duration_ms", elapsed.count()),
            });
            generation->logger->log("info", "response_sent", {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
                field_number("status_code", response.status_code),
                field_number("duration_ms", elapsed.count()),
                field_bool("streaming", false),
            });
            return sender(build_response(std::move(response)));
        }

        const auto& route = *lookup.entry;
        profile_id = route.profile->id;
        protocol_id = std::string(route.profile->handler->id());
        route_kind = route_kind_name(route.kind);
        protocol_handler = route.profile->handler;
        if (route.kind == RouteKind::Usage) {
            auto [response, forwarded] = handle_usage_request(
                generation, request, route, cancellation);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            generation->logger->log("info", "usage_completed", {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
                field_bool("forwarded", forwarded),
                field_string("upstream_url", route.upstream.base_url),
                field_string("upstream_path", route.upstream.path),
                field_number("status_code", response.status_code),
                field_number("duration_ms", elapsed.count()),
            });
            return sender(build_response(std::move(response)));
        }

        std::vector<LogField> request_fields = {
            field_string("request_id", request.request_id),
            field_number("generation_id", static_cast<long long>(generation->id)),
            field_string("profile_id", profile_id),
            field_string("protocol", protocol_id),
            field_string("route_kind", route_kind),
            field_string("method", request.method),
            field_string("local_path", lookup.canonical_path),
            field_string("query", request.query),
            field_string("client_ip", request.client_ip),
            field_raw("headers", headers_to_json(request.headers, logging.redact_sensitive)),
        };
        append_body_fields(request_fields, logging, request.body);
        generation->logger->log("info", "request_received", request_fields);

        RulePipelineResult pipeline_result;
        if (!route.profile->request_pipeline) {
            pipeline_result.ok = false;
            pipeline_result.error = RulePipelineError{
                500,
                "server_error",
                "runtime profile has no compiled request pipeline",
                {},
                {},
                "missing_compiled_pipeline",
            };
        } else {
            pipeline_result = route.profile->request_pipeline->apply(request.body);
        }
        for (const auto& trace : pipeline_result.traces) {
            generation->logger->log("info", "request_rule", {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
                field_string("rule_id", trace.rule_id),
                field_string("rule_type", trace.rule_type),
                field_bool("matched", trace.matched),
                field_bool("modified", trace.modified),
                field_string("reason", trace.reason),
                field_string("target", trace.summary.target),
                field_number("affected_count", static_cast<long long>(trace.summary.affected_count)),
                field_number("duration_us", static_cast<long long>(trace.duration_us)),
            });
        }
        if (!pipeline_result.ok) {
            const RulePipelineError fallback{
                500,
                "server_error",
                "request rule pipeline failed",
                {},
                {},
                "pipeline_failed",
            };
            const auto& pipeline_error = pipeline_result.error
                ? *pipeline_result.error
                : fallback;
            generation->logger->log("error", "request_error", {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
                field_string("rule_id", pipeline_error.rule_id),
                field_string("rule_type", pipeline_error.rule_type),
                field_string("reason", pipeline_error.reason),
                field_string("message", pipeline_error.message),
                field_string("type", pipeline_error.type),
                field_number("status_code", pipeline_error.status_code),
            });
            auto response = route.profile->handler->local_error(
                pipeline_error.status_code,
                pipeline_error.type,
                pipeline_error.message);
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
            std::vector<LogField> sent_fields = {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
                field_number("status_code", response.status_code),
                field_number("duration_ms", elapsed.count()),
                field_bool("streaming", false),
            };
            append_body_fields(sent_fields, logging, response.body);
            generation->logger->log("info", "response_sent", sent_fields);
            return sender(build_response(std::move(response)));
        }
        if (pipeline_result.rewritten_body) {
            request.body = std::move(*pipeline_result.rewritten_body);
        }

        HttpResponse response;
        bool streamed = false;
        std::size_t streamed_body_size = 0;
        std::size_t stream_chunk_count = 0;

        std::vector<LogField> upstream_request_fields = {
            field_string("request_id", request.request_id),
            field_number("generation_id", static_cast<long long>(generation->id)),
            field_string("profile_id", profile_id),
            field_string("protocol", protocol_id),
            field_string("route_kind", route_kind),
            field_string("method", request.method),
            field_string("upstream_url", route.upstream.base_url),
            field_string("upstream_path", route.upstream.path),
            field_string("query", request.query),
            field_string("upstream_proxy_mode", generation->transport->proxy_mode()),
            field_bool("rewrite_modified", pipeline_result.modified),
            field_number("rule_count", static_cast<long long>(pipeline_result.traces.size())),
            field_number("json_parse_count", static_cast<long long>(pipeline_result.parse_count)),
            field_number("json_serialize_count", static_cast<long long>(pipeline_result.serialize_count)),
            field_number("original_body_size", static_cast<long long>(pipeline_result.original_body_size)),
            field_number("rewritten_body_size", static_cast<long long>(pipeline_result.output_body_size)),
            field_raw("headers", headers_to_json(request.headers, logging.redact_sensitive)),
        };
        append_body_fields(upstream_request_fields, logging, request.body);
        generation->logger->log("info", "upstream_request", upstream_request_fields);

        try {
            response = generation->transport->forward_streaming(
                request,
                route.upstream,
                [&](const HttpResponse& headers_response) {
                    streamed = true;
                    metrics_->stream_started();
                    generation->logger->log("info", "upstream_response", {
                        field_string("request_id", request.request_id),
                        field_number("generation_id", static_cast<long long>(generation->id)),
                        field_string("profile_id", profile_id),
                        field_string("protocol", protocol_id),
                        field_string("route_kind", route_kind),
                        field_number("status_code", headers_response.status_code),
                        field_bool("streaming", true),
                        field_raw("headers", headers_to_json(
                            headers_response.headers, logging.redact_sensitive)),
                        field_number("body_size", 0),
                    });
                    return sender(build_stream_response_head(headers_response));
                },
                [&](const std::string& chunk) {
                    std::vector<LogField> chunk_fields = {
                        field_string("request_id", request.request_id),
                        field_number("generation_id", static_cast<long long>(generation->id)),
                        field_string("profile_id", profile_id),
                        field_string("protocol", protocol_id),
                        field_string("route_kind", route_kind),
                        field_number("chunk_sequence", static_cast<long long>(stream_chunk_count)),
                        field_number("chunk_size", static_cast<long long>(chunk.size())),
                    };
                    append_body_fields(chunk_fields, logging, chunk);
                    generation->logger->log("info", "stream_chunk", chunk_fields);
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
                generation->logger->log("info", "request_cancelled", {
                    field_string("request_id", request.request_id),
                    field_number("generation_id", static_cast<long long>(generation->id)),
                    field_string("profile_id", profile_id),
                    field_string("protocol", protocol_id),
                    field_string("route_kind", route_kind),
                    field_number("duration_ms", elapsed.count()),
                });
                return false;
            }
            metrics_->upstream_request_failed();
            generation->logger->log("error", "request_error", {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
                field_string("message", ex.what()),
                field_string("type", ex.type()),
                field_number("status_code", ex.status_code()),
            });
            if (streamed) {
                return true;
            }
            response = route.profile->handler->local_error(
                ex.status_code(), ex.type(), ex.what());
        }

        if (!streamed) {
            if (response.reason.empty()) {
                response.reason = reason_phrase(response.status_code);
            }
            std::vector<LogField> upstream_response_fields = {
                field_string("request_id", request.request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
                field_number("status_code", response.status_code),
                field_bool("streaming", false),
                field_raw("headers", headers_to_json(
                    response.headers, logging.redact_sensitive)),
            };
            append_body_fields(upstream_response_fields, logging, response.body);
            generation->logger->log("info", "upstream_response", upstream_response_fields);
        }

        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        std::vector<LogField> sent_fields = {
            field_string("request_id", request.request_id),
            field_number("generation_id", static_cast<long long>(generation->id)),
            field_string("profile_id", profile_id),
            field_string("protocol", protocol_id),
            field_string("route_kind", route_kind),
            field_number("status_code", response.status_code),
            field_number("duration_ms", elapsed.count()),
            field_bool("streaming", streamed),
            field_number("stream_chunk_count", static_cast<long long>(stream_chunk_count)),
            field_number("streamed_body_size", static_cast<long long>(streamed_body_size)),
            field_raw("headers", headers_to_json(response.headers, logging.redact_sensitive)),
        };
        if (!streamed) {
            append_body_fields(sent_fields, logging, response.body);
        }
        generation->logger->log("info", "response_sent", sent_fields);

        if (streamed) {
            return true;
        }
        return sender(build_response(std::move(response)));
    } catch (const ProxyError& ex) {
        if (ex.type() == "client_cancelled") {
            generation->logger->log("info", "request_cancelled", {
                field_string("request_id", request_id),
                field_number("generation_id", static_cast<long long>(generation->id)),
                field_string("profile_id", profile_id),
                field_string("protocol", protocol_id),
                field_string("route_kind", route_kind),
            });
            return false;
        }
        throw;
    } catch (const std::exception& ex) {
        generation->logger->log("error", "request_error", {
            field_string("request_id", request_id),
            field_number("generation_id", static_cast<long long>(generation->id)),
            field_string("profile_id", profile_id),
            field_string("protocol", protocol_id),
            field_string("route_kind", route_kind),
            field_string("message", ex.what()),
            field_string("type", "server_error"),
            field_number("status_code", 500),
        });

        HttpResponse response = protocol_handler
            ? protocol_handler->local_error(500, "server_error", "internal server error")
            : HttpResponse{
                  500,
                  reason_phrase(500),
                  {},
                  error_json("internal server error", "server_error"),
              };
        return sender(build_response(response));
    }
}

void Server::log_performance_snapshot(const std::string& reason) const {
    const auto snapshot = metrics_->snapshot();
    const auto generation = current_generation();
    generation->logger->log("info", "performance_snapshot", {
        field_number("generation_id", static_cast<long long>(generation->id)),
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
        field_number("log_writers_active", static_cast<long long>(snapshot.log_writers_active)),
        field_bool("log_writer_healthy", snapshot.log_writer_healthy != 0),
        field_number("max_log_batch_records", static_cast<long long>(snapshot.max_log_batch_records)),
        field_number("max_log_batch_bytes", static_cast<long long>(snapshot.max_log_batch_bytes)),
    });
}

std::pair<HttpResponse, bool> Server::handle_usage_request(
    const std::shared_ptr<RequestGeneration>& generation,
    const HttpRequest& request,
    const RouteEntry& route,
    const CancellationToken& cancellation) const {
    try {
        auto response = generation->transport->forward(
            request, route.upstream, cancellation);
        if (response.reason.empty()) {
            response.reason = reason_phrase(response.status_code);
        }
        return {std::move(response), true};
    } catch (const ProxyError& ex) {
        if (ex.type() == "client_cancelled") {
            throw;
        }
        metrics_->upstream_request_failed();
        generation->logger->log("error", "request_error", {
            field_string("request_id", request.request_id),
            field_number("generation_id", static_cast<long long>(generation->id)),
            field_string("profile_id", route.profile->id),
            field_string("protocol", std::string(route.profile->handler->id())),
            field_string("route_kind", route_kind_name(route.kind)),
            field_string("message", ex.what()),
            field_string("type", ex.type()),
            field_number("status_code", ex.status_code()),
        });
        return {
            route.profile->handler->local_error(
                ex.status_code(), ex.type(), ex.what()),
            false,
        };
    }
}

HttpResponse Server::handle_local_route_error(const RouteLookup& route) const {
    HttpResponse response;
    if (route.status == RouteLookupStatus::InvalidPath) {
        response.status_code = 400;
        response.body = error_json("invalid request path", "invalid_request_error");
    } else if (route.status == RouteLookupStatus::MethodNotAllowed) {
        response.status_code = 405;
        std::string allowed;
        for (const auto& method : route.allowed_methods) {
            if (!allowed.empty()) {
                allowed += ", ";
            }
            allowed += method;
        }
        response.headers.emplace_back("Allow", std::move(allowed));
        response.body = error_json("method not allowed", "invalid_request_error");
    } else {
        response.status_code = 404;
        response.body = error_json("unsupported route", "invalid_request_error");
    }
    response.reason = reason_phrase(response.status_code);
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
        stop_requested_.store(false, std::memory_order_release);
        fatal_error_.store(false, std::memory_order_release);
        console_shutdown_requested().store(false, std::memory_order_release);
        auto startup_generation = current_generation();
        const auto startup_snapshot = startup_generation->snapshot;
        const auto application = startup_snapshot->application;
        const auto log_path = startup_snapshot->log_path;
        const auto profile_count = startup_snapshot->profiles.size();
        const auto route_count = startup_snapshot->routes.size();
        WinsockRuntime winsock;
        std::string log_error;
        if (!startup_generation->logger->open(log_error)) {
            report_startup(false, log_error);
            std::cerr << log_error << "\n";
            return 1;
        }
        SetConsoleCtrlHandler(console_handler, TRUE);

        BoundListener listener(application.listener);
        std::string listener_error;
        if (!listener.open(listener_error)) {
            report_startup(false, listener_error);
            std::cerr << listener_error << "\n";
            SetConsoleCtrlHandler(console_handler, FALSE);
            return 1;
        }

        std::cout << "ccs-trans listening on http://"
                  << application.listener.host << ":" << application.listener.port << "\n";
        const bool startup_logged = startup_generation->logger->log("info", "server_start", {
            field_number("generation_id", static_cast<long long>(startup_generation->id)),
            field_number("listener_count", 1),
            field_string("listen_host", application.listener.host),
            field_number("listen_port", application.listener.port),
            field_number("profile_count", static_cast<long long>(profile_count)),
            field_number("route_count", static_cast<long long>(route_count)),
            field_string("log_path", log_path.string()),
            field_string("upstream_proxy_mode", startup_generation->transport->proxy_mode()),
            field_number("worker_threads", application.runtime.worker_threads),
            field_number("max_connections", application.runtime.max_connections),
            field_number("metrics_interval_ms", application.runtime.metrics_interval_ms),
            field_number("resolve_timeout_ms", application.timeouts.resolve_ms),
            field_number("connect_timeout_ms", application.timeouts.connect_ms),
            field_number("send_timeout_ms", application.timeouts.send_ms),
            field_number("response_header_timeout_ms", application.timeouts.response_header_ms),
            field_number("stream_idle_timeout_ms", application.timeouts.stream_idle_ms),
            field_number("total_timeout_ms", application.timeouts.total_ms),
        });
        std::string startup_log_error;
        if (!startup_logged || !startup_generation->logger->drain(startup_log_error)) {
            if (startup_log_error.empty()) {
                startup_log_error = "failed to write server_start log event";
            }
            report_startup(false, startup_log_error);
            std::cerr << startup_log_error << "\n";
            SetConsoleCtrlHandler(console_handler, FALSE);
            return 1;
        }
        startup_generation.reset();

        PeriodicReporter reporter(
            static_cast<int>(application.runtime.metrics_interval_ms), [this]() {
            log_performance_snapshot("periodic");
        });

        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::queue<ClientJob> client_queue;
        std::size_t open_connections = 0;
        std::size_t active_worker_count = 0;
        bool accepting_done = false;
        ClientCancellationMonitor cancellation_monitor(metrics_);

        std::vector<std::thread> workers;
        const auto worker_limit = static_cast<std::size_t>(application.runtime.worker_threads);
        workers.reserve(worker_limit);
        const auto worker_loop = [&]() {
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
                    ++active_worker_count;
                    const auto queue_wait = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - job.accepted_at);
                    metrics_->worker_started(
                        client_queue.size(),
                        static_cast<std::uint64_t>(queue_wait.count()));
                }
                try {
                    const auto request_generation = current_generation();
                    handle_client(
                        job.client,
                        this,
                        cancellation_monitor,
                        static_cast<std::size_t>(request_generation
                                ->runtime.application.runtime.max_request_body_size),
                        std::move(job.client_ip),
                        [this, request_generation](
                            const std::string& raw,
                            const std::string& client_ip,
                            const std::function<bool(const std::string&)>& sender,
                            const CancellationToken& cancellation) {
                            return process_with_generation(
                                request_generation, raw, client_ip, sender, cancellation);
                        });
                } catch (...) {
                    close_socket(job.client);
                    try {
                        log_request_error(
                            500, "worker_error", "request worker failed unexpectedly");
                    } catch (...) {
                    }
                }
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    --active_worker_count;
                    --open_connections;
                    metrics_->worker_finished(open_connections);
                }
            }
        };
        const auto grow_workers = [&](std::size_t target) {
            while (workers.size() < target) {
                workers.emplace_back(worker_loop);
            }
        };
        try {
            grow_workers(std::min<std::size_t>(worker_limit, 8));
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

        const auto accept_loop = [&]() noexcept {
            try {
                while (!stop_requested_.load(std::memory_order_acquire)
                    && !console_shutdown_requested().load(std::memory_order_acquire)) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(listener.socket(), &read_set);
                timeval wait_time{};
                wait_time.tv_usec = 100 * 1000;
                const int selected = select(0, &read_set, nullptr, nullptr, &wait_time);
                if (selected == 0) {
                    continue;
                }
                if (selected == SOCKET_ERROR) {
                    const int error = WSAGetLastError();
                    if (stop_requested_.load(std::memory_order_acquire)
                        || console_shutdown_requested().load(std::memory_order_acquire)) {
                        return;
                    }
                    log_request_error(
                        500,
                        "listener_error",
                        "listener wait failed: " + std::to_string(error));
                    stop_requested_.store(true, std::memory_order_release);
                    return;
                }
                sockaddr_storage client_addr{};
                int client_addr_len = sizeof(client_addr);
                SOCKET client = accept(
                    listener.socket(),
                    reinterpret_cast<sockaddr*>(&client_addr),
                    &client_addr_len);
                if (client == INVALID_SOCKET) {
                    const int error = WSAGetLastError();
                    if (stop_requested_.load(std::memory_order_acquire)
                        || console_shutdown_requested().load(std::memory_order_acquire)) {
                        return;
                    }
                    log_request_error(
                        500,
                        "listener_error",
                        "listener accept failed: " + std::to_string(error));
                    stop_requested_.store(true, std::memory_order_release);
                    return;
                }
                bool queued = false;
                bool queue_failed = false;
                bool worker_scale_failed = false;
                std::size_t worker_count_after_scale = 0;
                std::string client_ip;
                try {
                    client_ip = peer_ip(client_addr);
                } catch (...) {
                    queue_failed = true;
                }
                {
                    std::lock_guard<std::mutex> lock(queue_mutex);
                    if (!queue_failed
                        && open_connections < current_generation()
                            ->runtime.application.runtime.max_connections) {
                        try {
                            client_queue.push(ClientJob{
                                client,
                                std::move(client_ip),
                                std::chrono::steady_clock::now(),
                            });
                            ++open_connections;
                            metrics_->connection_accepted(
                                open_connections, client_queue.size());
                            const auto required_workers = std::min(
                                worker_limit,
                                active_worker_count + client_queue.size());
                            try {
                                grow_workers(required_workers);
                            } catch (...) {
                                worker_scale_failed = true;
                            }
                            worker_count_after_scale = workers.size();
                            queued = true;
                        } catch (...) {
                            queue_failed = true;
                        }
                    }
                }
                if (queue_failed) {
                    close_socket(client);
                    stop_requested_.store(true, std::memory_order_release);
                    try {
                        log_request_error(
                            500, "listener_queue_error", "failed to queue accepted connection");
                    } catch (...) {
                    }
                    return;
                }
                if (worker_scale_failed) {
                    const auto current = current_generation();
                    current->logger->log("error", "worker_scale_failed", {
                        field_number("generation_id", static_cast<long long>(current->id)),
                        field_number("current_worker_threads", static_cast<long long>(worker_count_after_scale)),
                        field_number("worker_thread_limit", static_cast<long long>(worker_limit)),
                    });
                }
                if (!queued) {
                    metrics_->connection_rejected();
                    reject_overloaded_client(client, this);
                    continue;
                }
                queue_cv.notify_one();
            }
            } catch (...) {
                stop_requested_.store(true, std::memory_order_release);
                try {
                    log_request_error(
                        500, "listener_error", "listener loop failed unexpectedly");
                } catch (...) {
                }
            }
        };

        std::vector<std::thread> acceptors;
        acceptors.reserve(1);
        try {
            acceptors.emplace_back(accept_loop);
        } catch (...) {
            stop_requested_.store(true, std::memory_order_release);
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
        listener.close();
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
        const auto stopping_generation = current_generation();
        stopping_generation->logger->log("info", "server_stop", {
            field_number("generation_id", static_cast<long long>(stopping_generation->id)),
            field_number("listener_count", 1),
        });
        std::string drain_error;
        if (!stopping_generation->logger->drain(drain_error)) {
            handle_logger_failure(drain_error);
        }
        SetConsoleCtrlHandler(console_handler, FALSE);
        return fatal_error_.load(std::memory_order_acquire) ? 1 : 0;
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
