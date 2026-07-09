#include "server.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>

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
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
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

std::string build_response(HttpResponse response) {
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
    response.headers.emplace_back("Content-Length", std::to_string(response.body.size()));
    response.headers.emplace_back("Connection", "close");

    std::ostringstream out;
    out << "HTTP/1.1 " << response.status_code << " " << response.reason << "\r\n";
    for (const auto& [name, value] : response.headers) {
        out << name << ": " << value << "\r\n";
    }
    out << "\r\n";
    out << response.body;
    return out.str();
}

#ifdef _WIN32

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

    return buffer.substr(0, body_start + expected_body);
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
        const auto request = parse_request(raw, client_ip);
        const auto response = build_response(server->handle_request(request));
        send_all(client, response);
    } catch (const std::exception& ex) {
        const std::string message = std::strcmp(ex.what(), "request body too large") == 0
            ? "request body too large"
            : "internal server error";
        const int status = std::strcmp(ex.what(), "request body too large") == 0 ? 413 : 500;
        HttpResponse response;
        response.status_code = status;
        response.reason = reason_phrase(status);
        response.body = error_json(message, status == 413 ? "invalid_request_error" : "server_error");
        const auto raw_response = build_response(response);
        send_all(client, raw_response);
    }
    close_socket(client);
}

#endif

} // namespace

Server::Server(AppConfig config)
    : config_(config)
    , proxy_(config_) {}

HttpResponse Server::handle_request(const HttpRequest& request) const {
    if (request.path != config_.responses_path && request.path != config_.chat_path) {
        HttpResponse response;
        response.status_code = 404;
        response.reason = reason_phrase(404);
        response.body = error_json("unsupported route", "invalid_request_error");
        return response;
    }

    if (request.method != "POST") {
        HttpResponse response;
        response.status_code = 405;
        response.reason = reason_phrase(405);
        response.headers.emplace_back("Allow", "POST");
        response.body = error_json("method not allowed", "invalid_request_error");
        return response;
    }

    try {
        const auto upstream_path = request.path == config_.responses_path
            ? config_.upstream_responses_path
            : config_.upstream_chat_path;
        auto response = proxy_.forward(request, upstream_path);
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

int Server::run() {
#ifndef _WIN32
    std::cerr << "ccs-trans currently supports the built-in HTTP server on Windows only\n";
    return 1;
#else
    try {
        WinsockRuntime winsock;

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
            return 1;
        }

        std::cout << "ccs-trans listening on http://" << config_.listen_host << ":" << config_.listen_port << "\n";

        while (true) {
            sockaddr_storage client_addr{};
            int client_addr_len = sizeof(client_addr);
            SOCKET client = accept(listen_socket, reinterpret_cast<sockaddr*>(&client_addr), &client_addr_len);
            if (client == INVALID_SOCKET) {
                continue;
            }
            std::thread(handle_client, client, this, config_.max_body_size, peer_ip(client_addr)).detach();
        }
    } catch (const std::exception& ex) {
        std::cerr << "server failed: " << ex.what() << "\n";
        return 1;
    }
#endif
}

} // namespace ccs
