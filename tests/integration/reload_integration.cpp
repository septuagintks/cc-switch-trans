#include "config/app_paths.hpp"
#include "config/profile_store.hpp"
#include "core/app_service.hpp"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <future>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class WinsockScope {
public:
    WinsockScope() {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            throw std::runtime_error("WSAStartup failed: " + std::to_string(result));
        }
    }

    ~WinsockScope() {
        WSACleanup();
    }
};

void close_socket(SOCKET socket) {
    if (socket != INVALID_SOCKET) {
        closesocket(socket);
    }
}

void send_all(SOCKET socket, const std::string& data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const int sent = send(
            socket,
            data.data() + offset,
            static_cast<int>(data.size() - offset),
            0);
        if (sent <= 0) {
            throw std::runtime_error("socket send failed: " + std::to_string(WSAGetLastError()));
        }
        offset += static_cast<std::size_t>(sent);
    }
}

std::size_t content_length(const std::string& headers) {
    const std::string name = "Content-Length:";
    const auto position = headers.find(name);
    if (position == std::string::npos) {
        return 0;
    }
    const auto value_start = position + name.size();
    const auto value_end = headers.find("\r\n", value_start);
    return static_cast<std::size_t>(std::stoull(headers.substr(value_start, value_end - value_start)));
}

std::string receive_http_message(SOCKET socket) {
    std::string data;
    char buffer[4096];
    std::size_t expected_size = 0;
    while (true) {
        const int received = recv(socket, buffer, sizeof(buffer), 0);
        if (received <= 0) {
            break;
        }
        data.append(buffer, static_cast<std::size_t>(received));
        const auto header_end = data.find("\r\n\r\n");
        if (header_end != std::string::npos && expected_size == 0) {
            expected_size = header_end + 4 + content_length(data.substr(0, header_end + 2));
        }
        if (expected_size != 0 && data.size() >= expected_size) {
            break;
        }
    }
    return data;
}

class MockUpstream {
public:
    MockUpstream(std::string marker, bool gated)
        : marker_(std::move(marker)), gated_(gated) {
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) {
            throw std::runtime_error("failed to create mock listener");
        }
        const BOOL exclusive = TRUE;
        setsockopt(
            listener_,
            SOL_SOCKET,
            SO_EXCLUSIVEADDRUSE,
            reinterpret_cast<const char*>(&exclusive),
            sizeof(exclusive));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR
            || listen(listener_, 8) == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            close_socket(listener_);
            listener_ = INVALID_SOCKET;
            throw std::runtime_error("failed to bind mock listener: " + std::to_string(error));
        }
        int address_size = sizeof(address);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&address), &address_size) == SOCKET_ERROR) {
            close_socket(listener_);
            listener_ = INVALID_SOCKET;
            throw std::runtime_error("failed to inspect mock listener");
        }
        port_ = ntohs(address.sin_port);
        try {
            thread_ = std::thread([this]() { run(); });
        } catch (...) {
            close_socket(listener_);
            listener_ = INVALID_SOCKET;
            throw;
        }
    }

    ~MockUpstream() {
        stopping_.store(true, std::memory_order_release);
        release();
        if (thread_.joinable()) {
            thread_.join();
        }
        close_socket(listener_);
    }

    std::uint16_t port() const {
        return port_;
    }

    void wait_until_received() {
        std::unique_lock<std::mutex> lock(mutex_);
        require(
            received_cv_.wait_for(lock, 5s, [this]() { return received_; }),
            "timed out waiting for upstream " + marker_);
    }

    void release() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            released_ = true;
        }
        release_cv_.notify_all();
    }

private:
    void run() noexcept {
        try {
            while (!stopping_.load(std::memory_order_acquire)) {
                fd_set read_set;
                FD_ZERO(&read_set);
                FD_SET(listener_, &read_set);
                timeval timeout{};
                timeout.tv_usec = 100 * 1000;
                const int selected = select(0, &read_set, nullptr, nullptr, &timeout);
                if (selected <= 0) {
                    continue;
                }
                SOCKET client = accept(listener_, nullptr, nullptr);
                if (client == INVALID_SOCKET) {
                    continue;
                }
                try {
                    (void)receive_http_message(client);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        received_ = true;
                    }
                    received_cv_.notify_all();
                    if (gated_) {
                        std::unique_lock<std::mutex> lock(mutex_);
                        release_cv_.wait(lock, [this]() {
                            return released_ || stopping_.load(std::memory_order_acquire);
                        });
                    }
                    const std::string body = "{\"marker\":\"" + marker_ + "\"}";
                    send_all(
                        client,
                        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "
                            + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
                } catch (...) {
                }
                close_socket(client);
            }
        } catch (...) {
        }
    }

    std::string marker_;
    bool gated_ = false;
    SOCKET listener_ = INVALID_SOCKET;
    std::uint16_t port_ = 0;
    std::atomic_bool stopping_{false};
    std::mutex mutex_;
    std::condition_variable received_cv_;
    std::condition_variable release_cv_;
    bool received_ = false;
    bool released_ = false;
    std::thread thread_;
};

std::uint16_t free_port() {
    SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socket_handle != INVALID_SOCKET, "failed to create free-port socket");
    try {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        require(
            bind(socket_handle, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != SOCKET_ERROR,
            "failed to reserve a free port");
        int address_size = sizeof(address);
        require(
            getsockname(socket_handle, reinterpret_cast<sockaddr*>(&address), &address_size) != SOCKET_ERROR,
            "failed to inspect a free port");
        const auto port = ntohs(address.sin_port);
        close_socket(socket_handle);
        return port;
    } catch (...) {
        close_socket(socket_handle);
        throw;
    }
}

std::string proxy_request(std::uint16_t port, const std::string& label) {
    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET, "failed to create proxy client");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        const int error = WSAGetLastError();
        close_socket(client);
        throw std::runtime_error("failed to connect to proxy: " + std::to_string(error));
    }
    try {
        const std::string body = "{}";
        send_all(
            client,
            "POST /v1/responses?case=" + label + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
        const auto response = receive_http_message(client);
        close_socket(client);
        require(response.find("HTTP/1.1 200") == 0, "proxy request did not return HTTP 200");
        return response;
    } catch (...) {
        close_socket(client);
        throw;
    }
}

void test_reload_generation_and_profile_io() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-reload-integration-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    std::string error;
    require(ccs::ensure_app_directories(paths, error), error);

    MockUpstream upstream_a("A", true);
    MockUpstream upstream_b("B", false);
    ccs::AppConfig config;
    config.responses_endpoint.listen_port = free_port();
    do {
        config.chat_endpoint.listen_port = free_port();
    } while (config.chat_endpoint.listen_port == config.responses_endpoint.listen_port);
    config.responses_endpoint.upstream_url = "http://127.0.0.1:" + std::to_string(upstream_a.port());
    config.chat_endpoint.upstream_url = config.responses_endpoint.upstream_url;
    config.log_path = paths.default_log_file;
    config.log_body = false;
    config.redact_sensitive = true;

    {
        ccs::AppService service(config);
        require(service.start(error), "service start failed: " + error);

        auto old_request = std::async(std::launch::async, [&]() {
            return proxy_request(config.responses_endpoint.listen_port, "old");
        });
        upstream_a.wait_until_received();

        ccs::ProfileStore store(paths);
        require(store.load(error), error);
        require(store.create("live", error), error);
        require(store.set("live", "responses-upstream-url", config.responses_endpoint.upstream_url, error), error);
        require(store.save(error), error);
        ccs::ProfileStore reloaded(paths);
        require(reloaded.load(error), error);
        std::string profile_json;
        require(reloaded.show_json("live", profile_json, error), error);
        require(profile_json.find(config.responses_endpoint.upstream_url) != std::string::npos,
            "saved profile was not reloaded while proxy was active");

        auto next_config = config;
        next_config.responses_endpoint.upstream_url =
            "http://127.0.0.1:" + std::to_string(upstream_b.port());
        require(service.reload(ccs::make_config_snapshot(next_config), error), "hot reload failed: " + error);

        const auto new_response = proxy_request(config.responses_endpoint.listen_port, "new");
        require(new_response.find("\"marker\":\"B\"") != std::string::npos,
            "new request did not use reloaded upstream B");

        upstream_a.release();
        require(old_request.wait_for(5s) == std::future_status::ready, "old request did not complete");
        const auto old_response = old_request.get();
        require(old_response.find("\"marker\":\"A\"") != std::string::npos,
            "in-flight request changed upstream during reload");

        service.stop();
        require(service.wait() == 0, "service did not stop cleanly");
    }

    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
}

} // namespace

int main() {
    try {
        WinsockScope winsock;
        test_reload_generation_and_profile_io();
        std::cout << "reload integration ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "reload integration failed: " << ex.what() << "\n";
        return 1;
    }
}
