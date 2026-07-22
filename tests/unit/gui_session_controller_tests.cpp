#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/tray/gui_session_controller.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
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
        "failed to resolve session-controller test executable");
    return std::filesystem::path(buffer.data());
}

std::wstring utf8_to_wide(const std::string& value) {
    const int length = MultiByteToWideChar(
        CP_UTF8,
        MB_ERR_INVALID_CHARS,
        value.data(),
        static_cast<int>(value.size()),
        nullptr,
        0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(
            CP_UTF8,
            MB_ERR_INVALID_CHARS,
            value.data(),
            static_cast<int>(value.size()),
            result.data(),
            length) != length) {
        return {};
    }
    return result;
}

class FrameReader {
public:
    explicit FrameReader(HANDLE handle) : handle_(handle) {}

    bool read(std::string& frame, std::chrono::milliseconds timeout = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (std::chrono::steady_clock::now() < deadline) {
            if (next_frame_ < frames_.size()) {
                frame = std::move(frames_[next_frame_++]);
                return true;
            }
            frames_.clear();
            next_frame_ = 0;

            DWORD available = 0;
            if (!PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
                return false;
            }
            if (available == 0) {
                std::this_thread::sleep_for(2ms);
                continue;
            }

            std::vector<std::uint8_t> buffer(
                std::min<DWORD>(available, 64U * 1024U));
            DWORD received = 0;
            if (!ReadFile(
                    handle_,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    &received,
                    nullptr)
                || received == 0) {
                return false;
            }
            ccs::gui_ipc::FrameError error;
            if (!decoder_.consume(
                    std::span<const std::uint8_t>(buffer.data(), received),
                    frames_,
                    error)) {
                return false;
            }
        }
        return false;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    ccs::gui_ipc::FrameDecoder decoder_;
    std::vector<std::string> frames_;
    std::size_t next_frame_ = 0;
};

bool write_frame(HANDLE handle, const ccs::gui_ipc::Envelope& envelope) {
    std::string content;
    std::string error;
    if (!ccs::gui_ipc::serialize_envelope(envelope, content, error)) return false;
    std::vector<std::uint8_t> frame;
    ccs::gui_ipc::FrameError frame_error;
    if (!ccs::gui_ipc::encode_frame(content, frame, frame_error)) return false;
    DWORD written = 0;
    return WriteFile(
               handle,
               frame.data(),
               static_cast<DWORD>(frame.size()),
               &written,
               nullptr)
        && written == frame.size();
}

HANDLE open_pipe(const std::wstring& name) {
    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline) {
        const HANDLE pipe = CreateFileW(
            name.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
            nullptr);
        if (pipe != INVALID_HANDLE_VALUE) return pipe;
        if (GetLastError() == ERROR_PIPE_BUSY) {
            (void)WaitNamedPipeW(name.c_str(), 50);
        }
        std::this_thread::sleep_for(10ms);
    }
    return INVALID_HANDLE_VALUE;
}

int run_gui_probe(HANDLE bootstrap_handle) {
    FrameReader bootstrap_reader(bootstrap_handle);
    std::string bootstrap_frame;
    if (!bootstrap_reader.read(bootstrap_frame)) {
        CloseHandle(bootstrap_handle);
        return 10;
    }
    CloseHandle(bootstrap_handle);
    ccs::gui_ipc::LaunchBootstrap bootstrap;
    std::string error;
    if (!ccs::gui_ipc::parse_launch_bootstrap(
            bootstrap_frame, bootstrap, error)
        || bootstrap.version != "0.8-test"
        || bootstrap.source_commit != "session-source"
        || bootstrap.instance_identity != "session-instance") {
        return 11;
    }
    const auto pipe_name = utf8_to_wide(bootstrap.pipe_name_utf8);
    if (pipe_name.empty()) return 12;
    const HANDLE pipe = open_pipe(pipe_name);
    if (pipe == INVALID_HANDLE_VALUE) return 13;
    ccs::gui_ipc::Hello hello{
        bootstrap.version,
        bootstrap.source_commit,
        bootstrap.instance_identity,
        bootstrap.handshake_token,
        GetCurrentProcessId(),
    };
    std::string hello_payload;
    if (!ccs::gui_ipc::serialize_hello(hello, hello_payload, error)) {
        CloseHandle(pipe);
        return 14;
    }
    ccs::gui_ipc::Envelope envelope;
    envelope.kind = ccs::gui_ipc::MessageKind::Hello;
    envelope.request_id = "controller-hello";
    envelope.source_commit = bootstrap.source_commit;
    envelope.payload_json = std::move(hello_payload);
    if (!write_frame(pipe, envelope)) {
        CloseHandle(pipe);
        return 15;
    }

    FrameReader pipe_reader(pipe);
    bool hello_accepted = false;
    bool snapshot_received = false;
    bool activate_received = false;
    bool shutdown_received = false;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline && !shutdown_received) {
        std::string response_frame;
        if (!pipe_reader.read(response_frame, 1s)) {
            CloseHandle(pipe);
            return 16;
        }
        ccs::gui_ipc::Envelope response;
        if (!ccs::gui_ipc::parse_envelope(response_frame, response, error)) {
            CloseHandle(pipe);
            return 17;
        }
        if (response.kind == ccs::gui_ipc::MessageKind::HelloResult) {
            ccs::gui_ipc::HelloResult hello_result;
            if (!ccs::gui_ipc::parse_hello_result(
                    response.payload_json, hello_result, error)
                || !hello_result.accepted) {
                CloseHandle(pipe);
                return 18;
            }
            hello_accepted = true;
        } else if (response.kind == ccs::gui_ipc::MessageKind::Snapshot) {
            ccs::gui_ipc::Snapshot snapshot;
            if (!ccs::gui_ipc::parse_snapshot(
                    response.payload_json, snapshot, error)
                || snapshot.revision != 9) {
                CloseHandle(pipe);
                return 19;
            }
            snapshot_received = true;
        } else if (response.kind == ccs::gui_ipc::MessageKind::Activate) {
            activate_received = true;
        } else if (response.kind == ccs::gui_ipc::MessageKind::Shutdown) {
            shutdown_received = true;
        }
    }
    if (!hello_accepted || !snapshot_received || !activate_received
        || !shutdown_received) {
        CloseHandle(pipe);
        return 20;
    }
    CloseHandle(pipe);
    return 0;
}

void test_controller_lifecycle() {
    const auto nonce = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    ccs::GuiIpcServerCallbacks callbacks;
    callbacks.snapshot_provider = [] {
        ccs::gui_ipc::Snapshot snapshot;
        snapshot.revision = 9;
        snapshot.application.state = "running";
        return snapshot;
    };
    ccs::GuiSessionController controller({
        std::filesystem::current_path() / ("session-controller-" + nonce),
        current_executable(),
        "0.8-test",
        "session-source",
        "session-instance",
    }, std::move(callbacks));
    std::string error;
    require(controller.start(error), error);
    require(controller.launch_or_activate(error), error);

    const auto deadline = std::chrono::steady_clock::now() + 3s;
    while (!controller.status().ipc.authenticated) {
        require(std::chrono::steady_clock::now() < deadline,
            "sidecar did not complete the controller handshake");
        std::this_thread::sleep_for(10ms);
    }
    require(controller.status().process_running,
        "controller lost the authenticated GUI process handle");
    require(controller.launch_or_activate(error), error);
    require(controller.shutdown(2s, error), error);
    require(!controller.status().ipc.running && !controller.status().process_running,
        "controller did not stop the pipe server and child process");
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc == 3 && std::string(argv[1]) == "--ipc-bootstrap-handle") {
        try {
            return run_gui_probe(reinterpret_cast<HANDLE>(
                static_cast<std::uintptr_t>(std::stoull(argv[2]))));
        } catch (...) {
            return 9;
        }
    }
    try {
        test_controller_lifecycle();
        std::cout << "GUI session controller tests ok\n";
        return 0;
    } catch (const std::exception& exception) {
        std::cerr << "GUI session controller tests failed: "
                  << exception.what() << '\n';
        return 1;
    }
}
