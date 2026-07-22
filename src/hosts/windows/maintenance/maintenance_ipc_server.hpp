#pragma once

#ifdef _WIN32

#include "gui_ipc/protocol_types.hpp"

#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace ccs {

struct MaintenanceIpcServerConfig {
    std::wstring pipe_name;
    std::wstring current_user_sid;
    std::string version;
    std::string source_commit;
};

struct MaintenanceIpcServerCallbacks {
    std::function<gui_ipc::MaintenanceResult(
        const gui_ipc::MaintenanceRequest&)> request_handler;
    std::function<void(std::string_view, std::string, std::uint64_t)>
        event_handler;
};

struct MaintenanceIpcServerStatus {
    bool running = false;
    bool connected = false;
    std::size_t accepted_requests = 0;
    std::size_t rejected_requests = 0;
};

class MaintenanceIpcServer {
public:
    MaintenanceIpcServer(
        MaintenanceIpcServerConfig config,
        MaintenanceIpcServerCallbacks callbacks);
    ~MaintenanceIpcServer();

    MaintenanceIpcServer(const MaintenanceIpcServer&) = delete;
    MaintenanceIpcServer& operator=(const MaintenanceIpcServer&) = delete;

    bool start(std::string& error);
    void stop() noexcept;

    [[nodiscard]] MaintenanceIpcServerStatus status() const;
    [[nodiscard]] const std::wstring& pipe_name() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ccs

#endif
