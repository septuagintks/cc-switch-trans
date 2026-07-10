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

bool is_responses_path(const AppConfig& config, const std::string& path) {
    const auto task = TaskRouter(config).route(path).task;
    return task != nullptr && task->kind == ApiTaskKind::Responses;
}

bool is_chat_path(const AppConfig& config, const std::string& path) {
    const auto task = TaskRouter(config).route(path).task;
    return task != nullptr && task->kind == ApiTaskKind::ChatCompletions;
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

void handle_client(SOCKET client, const Server* server, std::size_t max_body_size, std::string client_ip) {
    try {
        const auto raw = recv_request(client, max_body_size);
        server->process_raw_request_to_sender(raw, client_ip, [&](const std::string& data) {
            return send_all(client, data);
        });
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
        server->log_request_error(status, too_large ? "invalid_request_error" : "server_error", message);
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
    close_socket(client);
}

#endif

} // namespace

Server::Server(AppConfig config)
    : config_(config)
    , router_(config_)
    , proxy_(config_.timeout_ms, config_.max_response_body_size)
    , logger_(config_) {
#ifdef _WIN32
    shutdown_requested().store(false, std::memory_order_relaxed);
#endif
}

void Server::log_request_error(int status_code, const std::string& type, const std::string& message) const {
    logger_.log("error", "request_error", {
        field_string("request_id", make_request_id()),
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

std::string Server::process_raw_request(const std::string& raw, const std::string& client_ip) const {
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
    const std::function<bool(const std::string&)>& sender) const {
    const auto request_id = make_request_id();
    const auto started = std::chrono::steady_clock::now();

    try {
        auto request = parse_request(raw, client_ip);
        const auto route = router_.route(request.path);
        if (route.task != nullptr && route.task->kind == ApiTaskKind::Usage) {
            if (!route.task->enabled) {
                HttpResponse response;
                response.status_code = 404;
                response.reason = reason_phrase(404);
                response.body = error_json("unsupported route", "invalid_request_error");
                return sender(build_response(std::move(response)));
            }
            return sender(build_response(handle_usage_request(request, *route.task)));
        }

        request.request_id = request_id;
        std::vector<LogField> request_fields = {
            field_string("request_id", request.request_id),
            field_string("api_type", route.task == nullptr ? "unknown" : task_name(route.task->kind)),
            field_string("method", request.method),
            field_string("local_path", request.path),
            field_string("query", request.query),
            field_string("client_ip", request.client_ip),
            field_raw("headers", headers_to_json(request.headers, config_.redact_sensitive)),
        };
        append_body_fields(request_fields, config_, request.body);
        logger_.log("info", "request_received", request_fields);

        const bool json_proxy_request = (is_responses_path(config_, request.path) || is_chat_path(config_, request.path))
            && request.method == "POST"
            && has_json_content_type(request.headers);
        if (json_proxy_request) {
            const auto trim_bytes = leading_json_whitespace(request.body);
            if (trim_bytes > 0 && trim_bytes < request.body.size()) {
                request.body.erase(0, trim_bytes);
                logger_.log("debug", "request_body_normalized", {
                    field_string("request_id", request.request_id),
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
            && route.task->kind != ApiTaskKind::Usage
            && route.task->enabled
            && request.method == route.task->method;
        if (main_task_ready
            && std::find(route.task->transforms.begin(), route.task->transforms.end(), "remove_findcg_image_gen") != route.task->transforms.end()) {
            try {
                transform_result = findcg_transform_.apply(*route.task, request.body);
                if (transform_result.rewritten_body) {
                    request.body = std::move(*transform_result.rewritten_body);
                    transform_result.rewritten_body.reset();
                }
            } catch (const TransformError& ex) {
                logger_.log("error", "request_error", {
                    field_string("request_id", request.request_id),
                    field_string("task", task_name(route.task->kind)),
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
                field_string("method", request.method),
                field_string("upstream_url", route.task->upstream.base_url),
                field_string("upstream_path", route.task->upstream.path),
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
                    route.task->upstream,
                    [&](const HttpResponse& headers_response) {
                        streamed = true;
                        std::vector<LogField> upstream_response_fields = {
                            field_string("request_id", request.request_id),
                            field_string("task", task_name(route.task->kind)),
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
                            field_number("chunk_sequence", static_cast<long long>(stream_chunk_count)),
                            field_number("chunk_size", static_cast<long long>(chunk.size())),
                        };
                        append_body_fields(chunk_fields, config_, chunk);
                        logger_.log("info", "stream_chunk", chunk_fields);
                        ++stream_chunk_count;
                        streamed_body_size += chunk.size();
                        return sender(chunk);
                    });
            } catch (const ProxyError& ex) {
                logger_.log("error", "request_error", {
                    field_string("request_id", request.request_id),
                    field_string("task", task_name(route.task->kind)),
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
                    field_number("status_code", response.status_code),
                    field_bool("streaming", false),
                    field_raw("headers", headers_to_json(response.headers, config_.redact_sensitive)),
                };
                append_body_fields(upstream_response_fields, config_, response.body);
                logger_.log("info", "upstream_response", upstream_response_fields);
            }
        } else {
            response = handle_local_route_error(request);
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
    } catch (const std::exception& ex) {
        logger_.log("error", "request_error", {
            field_string("request_id", request_id),
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

HttpResponse Server::handle_usage_request(const HttpRequest& request, const TaskConfig& task) const {
    if (request.method != "GET") {
        HttpResponse response;
        response.status_code = 405;
        response.reason = reason_phrase(405);
        response.headers.emplace_back("Allow", "GET");
        response.body = error_json("method not allowed", "invalid_request_error");
        return response;
    }

    try {
        auto response = proxy_.forward(request, task.upstream);
        if (response.reason.empty()) {
            response.reason = reason_phrase(response.status_code);
        }
        return response;
    } catch (const ProxyError& ex) {
        HttpResponse response;
        response.status_code = ex.status_code();
        response.reason = reason_phrase(response.status_code);
        response.body = error_json(ex.what(), ex.type());
        return response;
    }
}

HttpResponse Server::handle_local_route_error(const HttpRequest& request) const {
    const auto route = router_.route(request.path);
    if (route.task == nullptr) {
        HttpResponse response;
        response.status_code = 404;
        response.reason = reason_phrase(404);
        response.body = error_json("unsupported route", "invalid_request_error");
        return response;
    }

    const auto& task = *route.task;
    if (!task.enabled) {
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

int Server::run() {
#ifndef _WIN32
    std::cerr << "ccs-trans currently supports the built-in HTTP server on Windows only\n";
    return 1;
#else
    try {
        WinsockRuntime winsock;
        std::string log_error;
        if (!logger_.open(log_error)) {
            std::cerr << log_error << "\n";
            return 1;
        }
        SetConsoleCtrlHandler(console_handler, TRUE);

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_PASSIVE;

        addrinfo* result = nullptr;
        const auto port = std::to_string(config_.listen_port);
        const int rc = getaddrinfo(config_.listen_host.c_str(), port.c_str(), &hints, &result);
        if (rc != 0 || result == nullptr) {
            std::cerr << "failed to resolve listen address: " << config_.listen_host << ":" << port << "\n";
            return 1;
        }

        SOCKET listen_socket = INVALID_SOCKET;
        for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            listen_socket = socket(ptr->ai_family, ptr->ai_socktype, ptr->ai_protocol);
            if (listen_socket == INVALID_SOCKET) {
                continue;
            }

            const BOOL yes = TRUE;
            setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&yes), sizeof(yes));

            if (bind(listen_socket, ptr->ai_addr, static_cast<int>(ptr->ai_addrlen)) == 0) {
                break;
            }

            close_socket(listen_socket);
            listen_socket = INVALID_SOCKET;
        }
        freeaddrinfo(result);

        if (listen_socket == INVALID_SOCKET) {
            std::cerr << "failed to bind " << config_.listen_host << ":" << config_.listen_port << "\n";
            return 1;
        }

        if (listen(listen_socket, kBacklog) == SOCKET_ERROR) {
            std::cerr << "failed to listen on " << config_.listen_host << ":" << config_.listen_port << "\n";
            close_socket(listen_socket);
            SetConsoleCtrlHandler(console_handler, FALSE);
            return 1;
        }

        const int accept_timeout_ms = 500;
        setsockopt(listen_socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&accept_timeout_ms), sizeof(accept_timeout_ms));

        std::cout << "ccs-trans listening on http://" << config_.listen_host << ":" << config_.listen_port << "\n";
        logger_.log("info", "server_start", {
            field_string("listen_host", config_.listen_host),
            field_number("listen_port", config_.listen_port),
            field_bool("responses_enabled", config_.responses.enabled),
            field_string("responses_upstream_url", config_.responses.upstream.base_url),
            field_string("responses_upstream_path", config_.responses.upstream.path),
            field_bool("chat_enabled", config_.chat_completions.enabled),
            field_string("chat_upstream_url", config_.chat_completions.upstream.base_url),
            field_string("chat_upstream_path", config_.chat_completions.upstream.path),
            field_bool("usage_enabled", config_.usage.enabled),
            field_string("log_path", config_.log_path.string()),
            field_number("worker_threads", static_cast<long long>(config_.worker_threads)),
            field_number("max_connections", static_cast<long long>(config_.max_connections)),
        });

        std::mutex queue_mutex;
        std::condition_variable queue_cv;
        std::queue<ClientJob> client_queue;
        std::size_t open_connections = 0;
        bool accepting_done = false;

        std::vector<std::thread> workers;
        workers.reserve(config_.worker_threads);
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
                    }
                    handle_client(job.client, this, config_.max_request_body_size, std::move(job.client_ip));
                    {
                        std::lock_guard<std::mutex> lock(queue_mutex);
                        --open_connections;
                    }
                }
            });
        }

        while (!shutdown_requested().load(std::memory_order_relaxed)) {
            sockaddr_storage client_addr{};
            int client_addr_len = sizeof(client_addr);
            SOCKET client = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
            if (client == INVALID_SOCKET) {
                const int error = WSAGetLastError();
                if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                    continue;
                }
                continue;
            }
            bool queued = false;
            {
                std::lock_guard<std::mutex> lock(queue_mutex);
                if (open_connections < config_.max_connections) {
                    ++open_connections;
                    client_queue.push(ClientJob{client, peer_ip(client_addr)});
                    queued = true;
                }
            }
            if (!queued) {
                reject_overloaded_client(client, this);
                continue;
            }
            queue_cv.notify_one();
        }

        close_socket(listen_socket);
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
        logger_.log("info", "server_stop", {
            field_string("listen_host", config_.listen_host),
            field_number("listen_port", config_.listen_port),
        });
        SetConsoleCtrlHandler(console_handler, FALSE);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "server failed: " << ex.what() << "\n";
        SetConsoleCtrlHandler(console_handler, FALSE);
        return 1;
    }
#endif
}

} // namespace ccs
