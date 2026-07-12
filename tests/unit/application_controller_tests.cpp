#include "app/application_controller.hpp"
#include "config/config_document.hpp"
#include "config/config_store.hpp"

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

class WinsockScope {
public:
    WinsockScope() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "WSAStartup failed");
    }

    ~WinsockScope() {
        WSACleanup();
    }
};

std::uint16_t reserve_free_port() {
    const SOCKET socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(socket_handle != INVALID_SOCKET, "failed to create port probe socket");

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(
                socket_handle,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0,
        "failed to bind port probe socket");

    int length = sizeof(address);
    require(getsockname(
                socket_handle,
                reinterpret_cast<sockaddr*>(&address),
                &length) == 0,
        "failed to inspect port probe socket");
    closesocket(socket_handle);
    return ntohs(address.sin_port);
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "failed to open test config");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(static_cast<bool>(output), "failed to write test config");
}

std::string serialize(const ccs::ConfigDocument& document) {
    std::string content;
    std::string error;
    const bool serialized = ccs::serialize_config_document(document, content, error);
    require(serialized, error);
    return content;
}

void test_application_controller_lifecycle() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-application-controller-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    std::string error;
    const bool directories_ready = ccs::ensure_app_directories(paths, error);
    require(directories_ready, error);

    auto document = ccs::make_default_config_document();
    document.application.listener.port = reserve_free_port();
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = "/v1/responses";
    profile.upstream.base_url = "http://127.0.0.1:1";
    profile.upstream.request_path = "/v1/responses";
    document.profiles.emplace("test", std::move(profile));
    const auto valid_config = serialize(document);
    write_file(paths.config_file, valid_config);

    ccs::ApplicationController controller(paths);
    auto status = controller.status();
    require(status.state == ccs::ApplicationState::Stopped, "controller starts stopped");
    const bool started = controller.start(error);
    require(started, "controller start failed: " + error);
    status = controller.status();
    require(status.state == ccs::ApplicationState::Running, "controller reports running");
    require(status.listener_port == document.application.listener.port,
        "controller reports the active listener");

    error.clear();
    require(!controller.start(error) && error.find("cannot start") != std::string::npos,
        "duplicate start is rejected deterministically");

    ccs::ApplicationController conflicting(paths);
    error.clear();
    require(!conflicting.start(error), "port owner conflict unexpectedly started");
    require(conflicting.status().state == ccs::ApplicationState::Faulted,
        "port owner conflict is faulted");

    write_file(paths.config_file, "{ invalid json");
    error.clear();
    require(!controller.reload(error), "invalid config unexpectedly reloaded");
    status = controller.status();
    require(status.state == ccs::ApplicationState::Running && !status.last_error.empty(),
        "failed reload preserves the running service and records the error");

    write_file(paths.config_file, valid_config);
    error.clear();
    const bool reloaded = controller.reload(error);
    require(reloaded, "valid config reload failed: " + error);
    require(controller.status().last_error.empty(), "successful reload clears the last error");

    error.clear();
    const bool stopped = controller.stop(error);
    require(stopped, "controller stop failed: " + error);
    const bool stopped_again = controller.stop(error);
    require(stopped_again, "idempotent stop failed: " + error);
    require(controller.status().state == ccs::ApplicationState::Stopped,
        "controller reports stopped");

    error.clear();
    const bool conflict_started = conflicting.start(error);
    require(conflict_started, "controller did not acquire released listener: " + error);
    const bool conflict_stopped = conflicting.stop(error);
    require(conflict_stopped, "conflicting controller stop failed: " + error);

    error.clear();
    const bool restarted = controller.start(error);
    require(restarted, "controller restart failed: " + error);
    const bool shut_down = controller.shutdown(error);
    require(shut_down, "controller shutdown failed: " + error);
    require(controller.status().state == ccs::ApplicationState::Shutdown,
        "controller reports shutdown");
    error.clear();
    require(!controller.start(error) && error.find("shut down") != std::string::npos,
        "shutdown controller rejects later commands");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    try {
        WinsockScope winsock;
        test_application_controller_lifecycle();
        std::cout << "application controller tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "application controller tests failed: " << ex.what() << "\n";
        return 1;
    }
}
