#pragma once

#ifdef _WIN32

#include "gui_ipc/protocol_types.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace ccs {

struct GuiIpcServerConfig {
    std::wstring pipe_name;
    std::wstring current_user_sid;
    std::string version;
    std::string source_commit;
    std::string instance_identity;
    std::size_t outbound_queue_capacity = gui_ipc::kDefaultOutboundQueueCapacity;
};

struct GuiSessionCredentials {
    std::string handshake_token;
    std::string session_id;
    std::optional<std::uint64_t> expected_process_id;
};

struct GuiIpcServerCallbacks {
    std::function<gui_ipc::Snapshot()> snapshot_provider;
    std::function<void(const gui_ipc::Envelope&, const gui_ipc::Command&)>
        command_handler;
    std::function<void(std::string_view, std::string, std::uint64_t)> event_handler;
};

struct GuiIpcServerStatus {
    bool running = false;
    bool connected = false;
    bool authenticated = false;
    std::uint64_t client_process_id = 0;
    std::size_t accepted_connections = 0;
    std::size_t rejected_connections = 0;
    std::size_t disconnects = 0;
    std::size_t outbound_rejected = 0;
    std::size_t outbound_coalesced = 0;
};

class GuiIpcServer {
public:
    GuiIpcServer(GuiIpcServerConfig config, GuiIpcServerCallbacks callbacks);
    ~GuiIpcServer();

    GuiIpcServer(const GuiIpcServer&) = delete;
    GuiIpcServer& operator=(const GuiIpcServer&) = delete;

    bool start(std::string& error);
    bool prepare_session(GuiSessionCredentials credentials, std::string& error);
    bool publish_state(gui_ipc::Snapshot snapshot);
    bool send_command_completion(
        const std::string& request_id,
        const gui_ipc::CommandStatus& status,
        std::string base_revision = {});
    bool request_activate();
    bool request_shutdown();
    void stop() noexcept;

    [[nodiscard]] GuiIpcServerStatus status() const;
    [[nodiscard]] const std::wstring& pipe_name() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ccs

#endif
