#pragma once

#ifdef _WIN32

#include "gui_ipc/session.hpp"
#include "hosts/windows/gui_bridge/gui_ipc_server.hpp"
#include "hosts/windows/gui_bridge/gui_outbound_channel.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace ccs {

class GuiIpcConnection {
public:
    GuiIpcConnection(
        HANDLE pipe,
        std::uint64_t client_process_id,
        gui_ipc::ServerSessionPolicy policy,
        GuiIpcServerCallbacks callbacks,
        std::size_t outbound_capacity);
    ~GuiIpcConnection();

    GuiIpcConnection(const GuiIpcConnection&) = delete;
    GuiIpcConnection& operator=(const GuiIpcConnection&) = delete;

    void run();
    void stop() noexcept;
    bool publish_state(gui_ipc::Snapshot snapshot);
    bool send_command_completion(
        const std::string& request_id,
        const gui_ipc::CommandStatus& status,
        std::string base_revision);
    bool send_activate();
    bool send_shutdown();

    [[nodiscard]] bool authenticated() const noexcept;
    [[nodiscard]] std::uint64_t client_process_id() const noexcept;
    [[nodiscard]] GuiOutboundStatus outbound_status() const;

private:
    bool handle_frame(std::string_view frame, bool& drain_before_close);
    bool handle_hello(
        const gui_ipc::Envelope& envelope,
        bool& drain_before_close);
    bool handle_authenticated(const gui_ipc::Envelope& envelope);
    bool send_protocol_rejection(
        const gui_ipc::Envelope& envelope,
        gui_ipc::ErrorCode error,
        std::string detail);
    gui_ipc::Snapshot current_snapshot() const;
    gui_ipc::Envelope server_envelope(
        gui_ipc::MessageKind kind,
        std::string request_id) const;
    std::uint64_t next_server_sequence();
    void event(std::string_view name, std::string detail = {}) const;

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    const std::uint64_t client_process_id_;
    GuiIpcServerCallbacks callbacks_;
    mutable std::mutex session_mutex_;
    gui_ipc::ServerSession session_;
    GuiOutboundChannel outbound_;
    std::atomic_bool authenticated_{false};
    std::atomic_bool stopping_{false};
};

} // namespace ccs

#endif
