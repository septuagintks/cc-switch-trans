#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/tray/gui_process_launcher.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::filesystem::path current_executable() {
    std::vector<wchar_t> buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(length != 0 && length < buffer.size(),
        "failed to resolve launcher test executable");
    return std::filesystem::path(buffer.data());
}

int run_bootstrap_probe(HANDLE input) {
    ccs::gui_ipc::FrameDecoder decoder;
    std::vector<std::string> frames;
    std::vector<std::uint8_t> buffer(7);
    while (frames.empty()) {
        DWORD read = 0;
        if (!ReadFile(
                input,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &read,
                nullptr)) {
            return 10;
        }
        if (read == 0) return 11;
        ccs::gui_ipc::FrameError frame_error;
        if (!decoder.consume(
                std::span<const std::uint8_t>(buffer.data(), read),
                frames,
                frame_error)) {
            return 12;
        }
    }
    if (frames.size() != 1) return 13;
    ccs::gui_ipc::LaunchBootstrap bootstrap;
    std::string error;
    if (!ccs::gui_ipc::parse_launch_bootstrap(frames.front(), bootstrap, error)) {
        return 14;
    }
    if (bootstrap.pipe_name_utf8 != R"(\\.\pipe\ccs-trans-launcher-test)"
        || bootstrap.version != "0.8-test"
        || bootstrap.source_commit != "launcher-source"
        || bootstrap.instance_identity != "launcher-instance"
        || bootstrap.handshake_token != "launcher-token") {
        return 15;
    }
    if (bootstrap.session_id == "slow-success") {
        Sleep(150);
        return 0;
    }
    return bootstrap.session_id == "exit-37" ? 37 : 16;
}

ccs::gui_ipc::LaunchBootstrap bootstrap(std::string session_id) {
    return {
        R"(\\.\pipe\ccs-trans-launcher-test)",
        "0.8-test",
        "launcher-source",
        "launcher-instance",
        "launcher-token",
        std::move(session_id),
    };
}

void test_suspended_launch_and_exit_tracking() {
    ccs::GuiProcessLauncher launcher(current_executable());
    std::string error;
    require(launcher.launch_suspended(bootstrap("slow-success"), error), error);
    require(launcher.running() && launcher.suspended()
            && launcher.process_id() != 0,
        "launcher did not retain the suspended child identity");
    require(!launcher.launch_suspended(bootstrap("slow-success"), error)
            && error.find("already running") != std::string::npos,
        "launcher accepted a duplicate GUI process");
    require(launcher.resume(error), error);
    require(!launcher.suspended(), "launcher retained suspended state after resume");

    bool exited = true;
    DWORD exit_code = 99;
    require(launcher.wait_for_exit(0ms, exited, exit_code, error), error);
    require(!exited && exit_code == STILL_ACTIVE,
        "zero-time wait did not report the running GUI process");
    require(launcher.wait_for_exit(5s, exited, exit_code, error), error);
    require(exited && exit_code == 0 && !launcher.running(),
        "launcher probe did not exit cleanly");

    require(launcher.launch_suspended(bootstrap("exit-37"), error), error);
    require(launcher.resume(error), error);
    require(launcher.wait_for_exit(5s, exited, exit_code, error), error);
    require(exited && exit_code == 37,
        "launcher discarded the real child process exit code");
}

void test_sibling_path() {
    require(ccs::sibling_gui_executable(LR"(C:\Apps\ccs-trans-tray.exe)")
            == std::filesystem::path(LR"(C:\Apps\ccs-trans-gui.exe)"),
        "GUI sibling path does not share the tray installation directory");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string(argv[1]) == "--ipc-bootstrap-handle") {
        try {
            const auto value = std::stoull(argv[2]);
            return run_bootstrap_probe(
                reinterpret_cast<HANDLE>(static_cast<std::uintptr_t>(value)));
        } catch (...) {
            return 9;
        }
    }
    try {
        test_suspended_launch_and_exit_tracking();
        test_sibling_path();
        std::cout << "GUI process launcher tests ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "GUI process launcher tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
