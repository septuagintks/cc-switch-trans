#include "config/app_paths.hpp"
#include "config/config_document.hpp"
#include "config/config_store.hpp"
#include "config/runtime_compiler.hpp"
#include "core/inflight_memory_budget.hpp"
#include "app/app_service.hpp"

#include <nlohmann/json.hpp>

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

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

std::string http_body(const std::string& message) {
    const auto header_end = message.find("\r\n\r\n");
    require(header_end != std::string::npos, "HTTP message is missing its header terminator");
    return message.substr(header_end + 4);
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

std::vector<nlohmann::json> read_log_events(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty()) {
            events.push_back(nlohmann::json::parse(line));
        }
    }
    return events;
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

    void wait_until_received(std::size_t count = 1) {
        std::unique_lock<std::mutex> lock(mutex_);
        require(
            received_cv_.wait_for(lock, 5s, [this, count]() {
                return requests_.size() >= count;
            }),
            "timed out waiting for upstream " + marker_);
    }

    std::string request_body(std::size_t index) const {
        std::lock_guard<std::mutex> lock(mutex_);
        require(index < requests_.size(), "upstream request index is out of range");
        return http_body(requests_[index]);
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
                    const auto request = receive_http_message(client);
                    {
                        std::lock_guard<std::mutex> lock(mutex_);
                        requests_.push_back(request);
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
    mutable std::mutex mutex_;
    std::condition_variable received_cv_;
    std::condition_variable release_cv_;
    std::vector<std::string> requests_;
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

std::string proxy_request(
    std::uint16_t port,
    const std::string& label,
    const std::string& path = "/v1/responses",
    const std::string& body = "{}",
    int expected_status = 200) {
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
        send_all(
            client,
            "POST " + path + "?case=" + label + " HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size())
                + "\r\nConnection: close\r\n\r\n" + body);
        const auto response = receive_http_message(client);
        close_socket(client);
        require(
            response.find("HTTP/1.1 " + std::to_string(expected_status)) == 0,
            "proxy request returned an unexpected status");
        return response;
    } catch (...) {
        close_socket(client);
        throw;
    }
}

ccs::ConfigDocument make_document(
    std::uint16_t port,
    const std::filesystem::path& log_path,
    const std::string& upstream_url) {
    auto document = ccs::make_default_config_document();
    document.application.listener.port = port;
    document.application.logging.path = log_path.generic_string();
    document.application.logging.body = false;
    document.application.logging.redact_sensitive = true;
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = "/v1/responses";
    profile.upstream.base_url = upstream_url;
    profile.upstream.request_path = "/v1/responses";
    document.profiles.emplace("live", std::move(profile));
    return document;
}

ccs::RuleDefinition set_marker_rule(const std::string& id, const std::string& value) {
    ccs::RuleDefinition rule;
    rule.id.value = id;
    rule.enabled = true;
    rule.type = "set_field";
    rule.options["path"] = "/marker";
    rule.options["value"] = value;
    return rule;
}

ccs::RuntimeSnapshotPtr compile(
    const ccs::ConfigDocument& document,
    const std::filesystem::path& application_root) {
    ccs::RuntimeCompiler compiler(application_root);
    ccs::RuntimeSnapshotPtr snapshot;
    std::string error;
    require(compiler.compile(document, {}, snapshot, error), error);
    return snapshot;
}

bool try_compile(
    const ccs::ConfigDocument& document,
    const std::filesystem::path& application_root,
    ccs::RuntimeSnapshotPtr& snapshot,
    std::string& error) {
    ccs::RuntimeCompiler compiler(application_root);
    return compiler.compile(document, {}, snapshot, error);
}

void test_stop_with_incomplete_request() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-incomplete-request-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    const auto proxy_port = free_port();
    std::string error;
    require(ccs::ensure_app_directories(paths, error), error);

    auto document = make_document(
        proxy_port,
        paths.default_log_file,
        "http://127.0.0.1:1");
    ccs::AppService service(compile(document, paths.root));
    require(service.start(error), "incomplete-request service start failed: " + error);

    SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET, "failed to create incomplete-request client");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(proxy_port);
    require(
        connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address))
            != SOCKET_ERROR,
        "failed to connect incomplete-request client");
    send_all(
        client,
        "POST /v1/responses HTTP/1.1\r\nHost: 127.0.0.1\r\n"
        "Content-Length: 100\r\nConnection: close\r\n\r\n{");
    std::this_thread::sleep_for(100ms);

    service.stop();
    auto waiter = std::async(std::launch::async, [&]() { return service.wait(); });
    const auto stopped = waiter.wait_for(2s) == std::future_status::ready;
    if (!stopped) {
        shutdown(client, SD_BOTH);
        close_socket(client);
        waiter.wait();
        throw std::runtime_error(
            "service stop remained blocked by an incomplete client request");
    }
    close_socket(client);
    require(waiter.get() == 0, "service with incomplete request did not stop cleanly");

    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
}

void test_stop_cancels_active_upstream_request() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-stop-active-request-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    std::string error;
    require(ccs::ensure_app_directories(paths, error), error);

    MockUpstream upstream("stop", true);
    const auto proxy_port = free_port();
    auto document = make_document(
        proxy_port,
        paths.default_log_file,
        "http://127.0.0.1:" + std::to_string(upstream.port()));
    ccs::AppService service(compile(document, paths.root));
    require(service.start(error), "active-request service start failed: " + error);

    auto request = std::async(std::launch::async, [&]() {
        return proxy_request(proxy_port, "stop-active");
    });
    upstream.wait_until_received();

    service.stop();
    auto waiter = std::async(std::launch::async, [&]() { return service.wait(); });
    const auto stopped = waiter.wait_for(2s) == std::future_status::ready;
    if (!stopped) {
        upstream.release();
        waiter.wait();
        try {
            (void)request.get();
        } catch (...) {
        }
        throw std::runtime_error(
            "service stop remained blocked by an active upstream request");
    }
    require(waiter.get() == 0, "service with an active request did not stop cleanly");
    require(
        request.wait_for(2s) == std::future_status::ready,
        "cancelled client request did not finish after service stop");
    try {
        (void)request.get();
    } catch (...) {
    }
    upstream.release();

    const auto events = read_log_events(paths.default_log_file);
    require(
        std::any_of(events.begin(), events.end(), [](const nlohmann::json& event) {
            return event.value("event", "") == "request_cancelled";
        }),
        "service-stop cancellation is recorded in the request log chain");

    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
}

void test_reload_generation_and_profile_io() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-reload-integration-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    const auto reloaded_log_path = paths.logs_directory / "reloaded.log";
    std::string error;
    require(ccs::ensure_app_directories(paths, error), error);

    MockUpstream upstream_a("A", true);
    MockUpstream upstream_b("B", false);
    const auto proxy_port = free_port();
    auto document = make_document(
        proxy_port,
        paths.default_log_file,
        "http://127.0.0.1:" + std::to_string(upstream_a.port()));
    document.profiles.at("live").rules.push_back(set_marker_rule("set-first", "first"));
    document.profiles.at("live").rules.push_back(set_marker_rule("set-second", "second"));

    {
        ccs::AppService service(compile(document, paths.root));
        require(service.start(error), "service start failed: " + error);

        auto old_request = std::async(std::launch::async, [&]() {
            return proxy_request(
                proxy_port, "old", "/v1/responses", R"({"marker":"input"})");
        });
        upstream_a.wait_until_received();
        require(nlohmann::json::parse(upstream_a.request_body(0)).at("marker") == "second",
            "old generation applies its original rule order");

        ccs::ConfigStore store(paths);
        require(store.load(error), error);
        require(store.save(document, error), error);

        auto next_document = document;
        next_document.profiles.at("live").upstream.base_url =
            "http://127.0.0.1:" + std::to_string(upstream_b.port());
        next_document.profiles.at("live").local.request_path = "/v2/responses";
        std::reverse(
            next_document.profiles.at("live").rules.begin(),
            next_document.profiles.at("live").rules.end());
        require(store.save(next_document, error), error);
        ccs::ConfigStore reloaded(paths);
        require(reloaded.load(error), error);
        require(reloaded.document().profiles.at("live").upstream.base_url
                == next_document.profiles.at("live").upstream.base_url,
            "saved profile was not reloaded while proxy was active");

        const auto before_invalid_save = read_file(paths.config_file);
        auto invalid_document = next_document;
        invalid_document.application.runtime.worker_threads = 0;
        error.clear();
        require(!store.save(invalid_document, error), "invalid config save unexpectedly succeeded");
        require(read_file(paths.config_file) == before_invalid_save,
            "failed config save leaves canonical bytes unchanged");

        require(service.reload(compile(next_document, paths.root), error), "hot reload failed: " + error);

        (void)proxy_request(proxy_port, "removed-route", "/v1/responses", "{}", 404);
        const auto new_response = proxy_request(
            proxy_port, "new", "/v2/responses", R"({"marker":"input"})");
        require(new_response.find("\"marker\":\"B\"") != std::string::npos,
            "new request did not use reloaded upstream B");
        upstream_b.wait_until_received();
        require(nlohmann::json::parse(upstream_b.request_body(0)).at("marker") == "first",
            "new generation applies the reloaded rule order");

        auto collision_document = next_document;
        collision_document.profiles.emplace(
            "collision", collision_document.profiles.at("live"));
        ccs::RuntimeSnapshotPtr rejected_snapshot;
        error.clear();
        require(
            !try_compile(collision_document, paths.root, rejected_snapshot, error)
                && error.find("route collision") != std::string::npos,
            "route collision candidate is rejected before generation swap");
        const auto after_rejected_reload = proxy_request(
            proxy_port, "after-reject", "/v2/responses", R"({"marker":"input"})");
        require(after_rejected_reload.find("\"marker\":\"B\"") != std::string::npos,
            "rejected candidate leaves the current generation serving traffic");

        const auto blocked_log_parent = root / "blocked-log-parent";
        {
            std::ofstream output(blocked_log_parent, std::ios::binary);
            output << "not a directory";
        }
        auto invalid_log_document = next_document;
        invalid_log_document.application.logging.path =
            (blocked_log_parent / "ccs-trans.log").generic_string();
        error.clear();
        require(!service.reload(compile(invalid_log_document, paths.root), error),
            "candidate with an unusable log path unexpectedly reloaded");
        require(service.status() == ccs::ServiceState::Running,
            "failed candidate logger leaves the current service running");
        const auto after_log_failure = proxy_request(
            proxy_port, "after-log-failure", "/v2/responses", R"({"marker":"input"})");
        require(after_log_failure.find("\"marker\":\"B\"") != std::string::npos,
            "failed candidate logger leaves the current generation serving traffic");

        auto log_path_document = next_document;
        log_path_document.application.logging.path = reloaded_log_path.generic_string();
        require(service.reload(compile(log_path_document, paths.root), error),
            "log path generation reload failed: " + error);
        const auto log_path_response = proxy_request(
            proxy_port, "log-path", "/v2/responses", R"({"marker":"input"})");
        require(log_path_response.find("\"marker\":\"B\"") != std::string::npos,
            "log path reload changed request forwarding");

        upstream_a.release();
        require(old_request.wait_for(5s) == std::future_status::ready, "old request did not complete");
        const auto old_response = old_request.get();
        require(old_response.find("\"marker\":\"A\"") != std::string::npos,
            "in-flight request changed upstream during reload");

        service.stop();
        require(service.wait() == 0, "service did not stop cleanly");
    }

    const auto events = read_log_events(paths.default_log_file);
    std::string old_request_id;
    std::string new_request_id;
    std::uint64_t old_generation = 0;
    std::uint64_t new_generation = 0;
    for (const auto& event : events) {
        if (event.value("event", "") != "request_received") {
            continue;
        }
        const auto query = event.value("query", "");
        if (query == "case=old") {
            old_request_id = event.at("request_id").get<std::string>();
            old_generation = event.at("generation_id").get<std::uint64_t>();
        } else if (query == "case=new") {
            new_request_id = event.at("request_id").get<std::string>();
            new_generation = event.at("generation_id").get<std::uint64_t>();
        }
    }
    require(!old_request_id.empty() && !new_request_id.empty(),
        "reload log contains both request chains");
    require(old_generation != 0 && new_generation != 0 && old_generation != new_generation,
        "reload assigns distinct observable generations");

    std::vector<std::string> old_rules;
    std::vector<std::string> new_rules;
    bool swap_logged = false;
    for (const auto& event : events) {
        if (event.value("event", "") == "request_rule") {
            auto& rules = event.value("request_id", "") == old_request_id
                ? old_rules
                : new_rules;
            if (event.value("request_id", "") == old_request_id
                || event.value("request_id", "") == new_request_id) {
                rules.push_back(event.at("rule_id").get<std::string>());
            }
        }
        if (event.value("event", "") == "config_reload"
            && event.value("mode", "") == "generation_swap"
            && event.value("previous_generation_id", std::uint64_t{0}) == old_generation
            && event.value("generation_id", std::uint64_t{0}) == new_generation) {
            swap_logged = true;
        }
    }
    require(old_rules == std::vector<std::string>({"set-first", "set-second"}),
        "old request logs the original rule order");
    require(new_rules == std::vector<std::string>({"set-second", "set-first"}),
        "new request logs the reloaded rule order");
    require(swap_logged, "generation swap log links previous and current generations");

    const auto reloaded_events = read_log_events(reloaded_log_path);
    std::uint64_t log_path_generation = 0;
    bool log_path_swap_logged = false;
    bool healthy_writer_snapshot = false;
    bool generation_metrics_drained = false;
    for (const auto& event : reloaded_events) {
        if (event.value("event", "") == "request_received"
            && event.value("query", "") == "case=log-path") {
            log_path_generation = event.value("generation_id", std::uint64_t{0});
        }
        if (event.value("event", "") == "config_reload"
            && event.value("mode", "") == "generation_swap"
            && event.value("previous_generation_id", std::uint64_t{0}) == new_generation) {
            log_path_swap_logged = true;
        }
        if (event.value("event", "") == "performance_snapshot"
            && event.value("reason", "") == "server_stop"
            && event.value("log_writers_active", std::uint64_t{0}) == 1
            && event.value("log_writer_healthy", false)
            && event.value("log_writer_failures", std::uint64_t{0}) >= 1) {
            healthy_writer_snapshot = true;
        }
        if (event.value("event", "") == "performance_snapshot"
            && event.value("reason", "") == "server_stop"
            && event.value("inflight_budget_bytes", std::uint64_t{0})
                == ccs::kDefaultInflightMemoryBudget
            && event.value("current_inflight_bytes", std::uint64_t{1}) == 0
            && event.value("current_generation_requests", std::uint64_t{1}) == 0
            && event.value("peak_generation_requests", std::uint64_t{0}) >= 1
            && event.value("current_retired_generations", std::uint64_t{1}) == 0
            && event.value("peak_retired_generations", std::uint64_t{0}) >= 1) {
            generation_metrics_drained = true;
        }
    }
    require(log_path_generation != 0 && log_path_generation != new_generation,
        "log path reload publishes another observable generation");
    require(log_path_swap_logged, "log path swap links its previous generation");
    require(healthy_writer_snapshot,
        "retired and rejected writers do not clear the active generation health metric");
    require(generation_metrics_drained,
        "generation and inflight metrics retain peaks and drain at server stop");

    std::error_code remove_error;
    std::filesystem::remove_all(root, remove_error);
}

} // namespace

int main() {
    try {
        WinsockScope winsock;
        test_stop_with_incomplete_request();
        test_stop_cancels_active_upstream_request();
        test_reload_generation_and_profile_io();
        std::cout << "reload integration ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "reload integration failed: " << ex.what() << "\n";
        return 1;
    }
}
