#include "ipc/bootstrap_reader.hpp"

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <span>
#include <vector>

namespace ccs_trans::gui {

namespace {

std::string windows_error(const char* action, DWORD code) {
    LPSTR message = nullptr;
    const DWORD size = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPSTR>(&message),
        0,
        nullptr);
    std::string detail = action;
    if (size != 0 && message != nullptr) {
        detail += ": ";
        detail.append(message, size);
        while (!detail.empty()
               && (detail.back() == '\r' || detail.back() == '\n')) {
            detail.pop_back();
        }
    } else {
        detail += " (Windows error " + std::to_string(code) + ")";
    }
    if (message != nullptr) LocalFree(message);
    return detail;
}

} // namespace

bool bootstrap_handle_from_arguments(
    const QStringList& arguments,
    std::uintptr_t& handle,
    QString& error) {
    handle = 0;
    error.clear();
    const auto option = arguments.indexOf(QStringLiteral("--ipc-bootstrap-handle"));
    if (option < 0) {
        error = QStringLiteral("missing --ipc-bootstrap-handle");
        return false;
    }
    if (option + 1 >= arguments.size()) {
        error = QStringLiteral("missing inherited bootstrap handle value");
        return false;
    }
    bool parsed = false;
    const auto value = arguments.at(option + 1).toULongLong(&parsed, 10);
    if (!parsed || value == 0) {
        error = QStringLiteral("invalid inherited bootstrap handle value");
        return false;
    }
    handle = static_cast<std::uintptr_t>(value);
    return true;
}

bool read_launch_bootstrap(
    std::uintptr_t inherited_handle,
    ccs::gui_ipc::LaunchBootstrap& bootstrap,
    std::string& error) {
    error.clear();
    bootstrap = {};
    HANDLE handle = reinterpret_cast<HANDLE>(inherited_handle);
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
        error = "invalid inherited GUI bootstrap handle";
        return false;
    }

    ccs::gui_ipc::FrameDecoder decoder;
    std::array<std::uint8_t, 4096> buffer{};
    std::vector<std::string> frames;
    while (frames.empty()) {
        DWORD received = 0;
        if (!ReadFile(
                handle,
                buffer.data(),
                static_cast<DWORD>(buffer.size()),
                &received,
                nullptr)) {
            const auto code = GetLastError();
            CloseHandle(handle);
            error = windows_error("failed to read GUI launch bootstrap", code);
            return false;
        }
        if (received == 0) {
            CloseHandle(handle);
            error = "GUI launch bootstrap pipe closed before a complete frame";
            return false;
        }
        ccs::gui_ipc::FrameError frame_error;
        if (!decoder.consume(
                std::span<const std::uint8_t>(buffer.data(), received),
                frames,
                frame_error)) {
            CloseHandle(handle);
            error = "GUI launch bootstrap frame rejected: "
                + std::string(ccs::gui_ipc::frame_error_name(frame_error));
            return false;
        }
    }
    CloseHandle(handle);
    if (frames.size() != 1 || decoder.buffered_bytes() != 0) {
        error = "GUI launch bootstrap must contain exactly one frame";
        return false;
    }
    if (!ccs::gui_ipc::parse_launch_bootstrap(frames.front(), bootstrap, error)) {
        return false;
    }
    if (bootstrap.pipe_name_utf8.empty() || bootstrap.version.empty()
        || bootstrap.source_commit.empty() || bootstrap.instance_identity.empty()
        || bootstrap.handshake_token.empty() || bootstrap.session_id.empty()) {
        error = "GUI launch bootstrap is incomplete";
        return false;
    }
    return true;
}

} // namespace ccs_trans::gui
