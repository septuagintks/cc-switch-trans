#pragma once

#ifdef _WIN32

#include "hosts/windows/gui_bridge/gui_ipc_server.hpp"
#include "hosts/windows/platform/gui_pipe_security.hpp"
#include "hosts/windows/tray/gui_process_launcher.hpp"

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>

namespace ccs {

struct GuiSessionControllerConfig {
    std::filesystem::path config_root;
    std::filesystem::path gui_executable;
    std::string version;
    std::string source_commit;
    std::string instance_identity;
    std::size_t outbound_queue_capacity =
        gui_ipc::kDefaultOutboundQueueCapacity;
};

struct GuiSessionControllerStatus {
    GuiIpcServerStatus ipc;
    bool process_running = false;
    bool process_suspended = false;
    std::uint64_t process_id = 0;
};

class GuiSessionController {
public:
    GuiSessionController(
        GuiSessionControllerConfig config,
        GuiIpcServerCallbacks callbacks);
    ~GuiSessionController();

    GuiSessionController(const GuiSessionController&) = delete;
    GuiSessionController& operator=(const GuiSessionController&) = delete;

    bool start(std::string& error);
    bool launch_or_activate(std::string& error);
    bool publish_state(gui_ipc::Snapshot snapshot);
    bool send_command_completion(
        const std::string& request_id,
        const gui_ipc::CommandStatus& status,
        std::string base_revision = {});
    bool request_activate();
    bool shutdown(
        std::chrono::milliseconds timeout,
        std::string& error);

    [[nodiscard]] GuiSessionControllerStatus status() const;
    [[nodiscard]] const GuiPipeIdentity& identity() const noexcept;

private:
    static std::string pipe_name_utf8(const std::wstring& pipe_name);

    GuiSessionControllerConfig config_;
    GuiIpcServerCallbacks callbacks_;
    GuiPipeIdentity identity_;
    GuiProcessLauncher launcher_;
    std::unique_ptr<GuiIpcServer> server_;
};

} // namespace ccs

#endif
