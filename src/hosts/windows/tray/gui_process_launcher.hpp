#pragma once

#ifdef _WIN32

#include "gui_ipc/protocol_types.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace ccs {

class GuiProcessLauncher {
public:
    explicit GuiProcessLauncher(std::filesystem::path executable);
    ~GuiProcessLauncher();

    GuiProcessLauncher(const GuiProcessLauncher&) = delete;
    GuiProcessLauncher& operator=(const GuiProcessLauncher&) = delete;

    bool launch_suspended(
        const gui_ipc::LaunchBootstrap& bootstrap,
        std::string& error);
    bool resume(std::string& error);
    bool wait_for_exit(
        std::chrono::milliseconds timeout,
        bool& exited,
        DWORD& exit_code,
        std::string& error);
    bool terminate(std::string& error) noexcept;

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] bool suspended() const noexcept;
    [[nodiscard]] std::uint64_t process_id() const noexcept;
    [[nodiscard]] const std::filesystem::path& executable() const noexcept;

private:
    bool start_diagnostic_reader(std::string& error);
    std::string finish_diagnostic_reader(bool cancel) noexcept;
    void close_handles() noexcept;
    bool refresh_process_state() noexcept;

    std::filesystem::path executable_;
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    HANDLE diagnostic_read_ = nullptr;
    std::thread diagnostic_thread_;
    std::mutex diagnostic_mutex_;
    std::string diagnostic_output_;
    bool diagnostic_truncated_ = false;
    DWORD process_id_ = 0;
    bool suspended_ = false;
};

std::filesystem::path sibling_gui_executable(
    const std::filesystem::path& tray_executable);

} // namespace ccs

#endif
