#include "hosts/windows/tray/gui_process_launcher.hpp"

#ifdef _WIN32

#include "gui_ipc/frame_codec.hpp"
#include "gui_ipc/json_codec.hpp"
#include "hosts/windows/windows_error.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace ccs {

namespace {

std::wstring quoted(const std::filesystem::path& path) {
    std::wstring value = path.native();
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const auto character : value) {
        if (character == L'\\') {
            ++slashes;
            continue;
        }
        if (character == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            slashes = 0;
            continue;
        }
        result.append(slashes, L'\\');
        slashes = 0;
        result.push_back(character);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool write_bootstrap(
    HANDLE handle,
    const gui_ipc::LaunchBootstrap& bootstrap,
    std::string& error) {
    std::string payload;
    if (!gui_ipc::serialize_launch_bootstrap(bootstrap, payload, error)) return false;
    std::vector<std::uint8_t> frame;
    gui_ipc::FrameError frame_error;
    if (!gui_ipc::encode_frame(payload, frame, frame_error)) {
        error = "failed to frame GUI launch bootstrap: "
            + std::string(gui_ipc::frame_error_name(frame_error));
        return false;
    }
    std::size_t offset = 0;
    while (offset < frame.size()) {
        DWORD written = 0;
        if (!WriteFile(
                handle,
                frame.data() + offset,
                static_cast<DWORD>(frame.size() - offset),
                &written,
                nullptr)) {
            error = windows_error_message(
                "failed to write GUI launch bootstrap", GetLastError());
            return false;
        }
        if (written == 0) {
            error = "GUI launch bootstrap write returned zero bytes";
            return false;
        }
        offset += written;
    }
    return true;
}

} // namespace

GuiProcessLauncher::GuiProcessLauncher(std::filesystem::path executable)
    : executable_(std::move(executable)) {}

GuiProcessLauncher::~GuiProcessLauncher() {
    std::string error;
    (void)terminate(error);
    close_handles();
}

bool GuiProcessLauncher::launch_suspended(
    const gui_ipc::LaunchBootstrap& bootstrap,
    std::string& error) {
    error.clear();
    if (refresh_process_state()) {
        error = "ccs-trans GUI process is already running";
        return false;
    }
    std::error_code file_error;
    const bool executable_exists = std::filesystem::is_regular_file(
        executable_, file_error);
    if (!executable_exists) {
        error = "ccs-trans GUI executable is missing: " + executable_.string();
        if (file_error) error += ": " + file_error.message();
        return false;
    }

    SECURITY_ATTRIBUTES pipe_security{};
    pipe_security.nLength = sizeof(pipe_security);
    pipe_security.bInheritHandle = TRUE;
    HANDLE bootstrap_read = nullptr;
    HANDLE bootstrap_write = nullptr;
    if (!CreatePipe(&bootstrap_read, &bootstrap_write, &pipe_security, 64U * 1024U)) {
        error = windows_error_message(
            "failed to create GUI bootstrap pipe", GetLastError());
        return false;
    }
    HANDLE diagnostic_read = nullptr;
    HANDLE diagnostic_write = nullptr;
    if (!CreatePipe(
            &diagnostic_read, &diagnostic_write, &pipe_security, 64U * 1024U)) {
        error = windows_error_message(
            "failed to create GUI diagnostic pipe", GetLastError());
        CloseHandle(bootstrap_read);
        CloseHandle(bootstrap_write);
        return false;
    }
    if (!SetHandleInformation(diagnostic_read, HANDLE_FLAG_INHERIT, 0)) {
        error = windows_error_message(
            "failed to restrict GUI diagnostic handle inheritance", GetLastError());
        CloseHandle(bootstrap_read);
        CloseHandle(bootstrap_write);
        CloseHandle(diagnostic_read);
        CloseHandle(diagnostic_write);
        return false;
    }
    if (!SetHandleInformation(bootstrap_write, HANDLE_FLAG_INHERIT, 0)) {
        error = windows_error_message(
            "failed to restrict GUI bootstrap handle inheritance", GetLastError());
        CloseHandle(bootstrap_read);
        CloseHandle(bootstrap_write);
        CloseHandle(diagnostic_read);
        CloseHandle(diagnostic_write);
        return false;
    }

    SIZE_T attribute_bytes = 0;
    if (InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes)
            != FALSE
        || GetLastError() != ERROR_INSUFFICIENT_BUFFER
        || attribute_bytes == 0) {
        error = windows_error_message(
            "failed to size GUI process attribute list", GetLastError());
        CloseHandle(bootstrap_read);
        CloseHandle(bootstrap_write);
        CloseHandle(diagnostic_read);
        CloseHandle(diagnostic_write);
        return false;
    }
    auto attribute_storage = std::make_unique<std::uint8_t[]>(attribute_bytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        attribute_storage.get());
    if (!InitializeProcThreadAttributeList(
            attributes, 1, 0, &attribute_bytes)) {
        error = windows_error_message(
            "failed to initialize GUI process attribute list", GetLastError());
        CloseHandle(bootstrap_read);
        CloseHandle(bootstrap_write);
        CloseHandle(diagnostic_read);
        CloseHandle(diagnostic_write);
        return false;
    }
    std::array inherited_handles = {bootstrap_read, diagnostic_write};
    if (!UpdateProcThreadAttribute(
            attributes,
            0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles.data(),
            sizeof(inherited_handles),
            nullptr,
            nullptr)) {
        error = windows_error_message(
            "failed to restrict GUI process handle inheritance", GetLastError());
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(bootstrap_read);
        CloseHandle(bootstrap_write);
        CloseHandle(diagnostic_read);
        CloseHandle(diagnostic_write);
        return false;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = bootstrap_read;
    startup.StartupInfo.hStdOutput = diagnostic_write;
    startup.StartupInfo.hStdError = diagnostic_write;
    startup.lpAttributeList = attributes;
    std::wstring command_line = quoted(executable_)
        + L" --ipc-bootstrap-handle "
        + std::to_wstring(reinterpret_cast<std::uintptr_t>(bootstrap_read));
    std::vector<wchar_t> mutable_command(command_line.begin(), command_line.end());
    mutable_command.push_back(L'\0');
    PROCESS_INFORMATION process{};
    const BOOL created = CreateProcessW(
        executable_.c_str(),
        mutable_command.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT,
        nullptr,
        executable_.parent_path().c_str(),
        &startup.StartupInfo,
        &process);
    const DWORD create_error = created ? ERROR_SUCCESS : GetLastError();
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(bootstrap_read);
    CloseHandle(diagnostic_write);
    if (!created) {
        CloseHandle(bootstrap_write);
        CloseHandle(diagnostic_read);
        error = windows_error_message(
            "failed to create ccs-trans GUI process", create_error);
        return false;
    }
    process_ = process.hProcess;
    thread_ = process.hThread;
    diagnostic_read_ = diagnostic_read;
    process_id_ = process.dwProcessId;
    suspended_ = true;
    if (!start_diagnostic_reader(error)) {
        (void)TerminateProcess(process_, 1);
        (void)WaitForSingleObject(process_, 5000);
        close_handles();
        CloseHandle(bootstrap_write);
        return false;
    }

    const bool bootstrap_written = write_bootstrap(
        bootstrap_write, bootstrap, error);
    CloseHandle(bootstrap_write);
    if (!bootstrap_written) {
        (void)TerminateProcess(process_, 1);
        (void)WaitForSingleObject(process_, 5000);
        close_handles();
        return false;
    }
    return true;
}

bool GuiProcessLauncher::resume(std::string& error) {
    error.clear();
    if (!refresh_process_state() || !suspended_ || thread_ == nullptr) {
        error = "ccs-trans GUI process is not suspended";
        return false;
    }
    if (ResumeThread(thread_) == static_cast<DWORD>(-1)) {
        error = windows_error_message(
            "failed to resume ccs-trans GUI process", GetLastError());
        return false;
    }
    CloseHandle(thread_);
    thread_ = nullptr;
    suspended_ = false;
    return true;
}

bool GuiProcessLauncher::wait_for_exit(
    std::chrono::milliseconds timeout,
    bool& exited,
    DWORD& exit_code,
    std::string& error) {
    error.clear();
    exited = false;
    exit_code = STILL_ACTIVE;
    if (process_ == nullptr) {
        exited = true;
        exit_code = 0;
        return true;
    }
    const auto bounded = std::clamp<long long>(
        timeout.count(), 0, static_cast<long long>(MAXDWORD) - 1);
    const auto wait = WaitForSingleObject(process_, static_cast<DWORD>(bounded));
    if (wait == WAIT_TIMEOUT) return true;
    if (wait != WAIT_OBJECT_0) {
        error = windows_error_message(
            "failed to wait for ccs-trans GUI process", GetLastError());
        return false;
    }
    if (!GetExitCodeProcess(process_, &exit_code)) {
        error = windows_error_message(
            "failed to read ccs-trans GUI process exit code", GetLastError());
        return false;
    }
    error = finish_diagnostic_reader(false);
    exited = true;
    close_handles();
    return true;
}

bool GuiProcessLauncher::terminate(std::string& error) noexcept {
    error.clear();
    if (!refresh_process_state()) return true;
    if (!TerminateProcess(process_, 1)) {
        error = windows_error_message(
            "failed to terminate ccs-trans GUI process", GetLastError());
        return false;
    }
    (void)WaitForSingleObject(process_, 5000);
    close_handles();
    return true;
}

bool GuiProcessLauncher::running() const noexcept {
    if (process_ == nullptr) return false;
    DWORD code = 0;
    return GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE;
}

bool GuiProcessLauncher::suspended() const noexcept { return suspended_ && running(); }
std::uint64_t GuiProcessLauncher::process_id() const noexcept { return process_id_; }
const std::filesystem::path& GuiProcessLauncher::executable() const noexcept {
    return executable_;
}

bool GuiProcessLauncher::start_diagnostic_reader(std::string& error) {
    error.clear();
    diagnostic_output_.clear();
    diagnostic_truncated_ = false;
    const HANDLE handle = diagnostic_read_;
    try {
        diagnostic_thread_ = std::thread([this, handle] {
            constexpr std::size_t maximum_diagnostic_bytes = 64U * 1024U;
            std::array<char, 4096> buffer{};
            for (;;) {
                DWORD received = 0;
                if (!ReadFile(
                        handle,
                        buffer.data(),
                        static_cast<DWORD>(buffer.size()),
                        &received,
                        nullptr)
                    || received == 0) {
                    break;
                }
                std::lock_guard<std::mutex> lock(diagnostic_mutex_);
                const auto remaining = maximum_diagnostic_bytes
                    - std::min(maximum_diagnostic_bytes, diagnostic_output_.size());
                const auto accepted = std::min<std::size_t>(remaining, received);
                diagnostic_output_.append(buffer.data(), accepted);
                diagnostic_truncated_ = diagnostic_truncated_
                    || accepted < received;
            }
        });
    } catch (const std::exception& exception) {
        error = "failed to start GUI diagnostic reader: "
            + std::string(exception.what());
        return false;
    }
    return true;
}

std::string GuiProcessLauncher::finish_diagnostic_reader(bool cancel) noexcept {
    const HANDLE handle = diagnostic_read_;
    diagnostic_read_ = nullptr;
    if (cancel && handle != nullptr) {
        (void)CancelIoEx(handle, nullptr);
        CloseHandle(handle);
    }
    if (diagnostic_thread_.joinable()) diagnostic_thread_.join();
    if (!cancel && handle != nullptr) CloseHandle(handle);

    std::string output;
    {
        std::lock_guard<std::mutex> lock(diagnostic_mutex_);
        output = std::move(diagnostic_output_);
        diagnostic_output_.clear();
        if (diagnostic_truncated_) {
            output += "\n[GUI diagnostic output truncated]";
        }
        diagnostic_truncated_ = false;
    }
    while (!output.empty()
           && (output.back() == '\r' || output.back() == '\n'
               || output.back() == ' ' || output.back() == '\t')) {
        output.pop_back();
    }
    return output;
}

void GuiProcessLauncher::close_handles() noexcept {
    (void)finish_diagnostic_reader(true);
    if (thread_ != nullptr) CloseHandle(thread_);
    if (process_ != nullptr) CloseHandle(process_);
    thread_ = nullptr;
    process_ = nullptr;
    process_id_ = 0;
    suspended_ = false;
}

bool GuiProcessLauncher::refresh_process_state() noexcept {
    if (process_ == nullptr) return false;
    DWORD code = 0;
    if (GetExitCodeProcess(process_, &code) && code == STILL_ACTIVE) return true;
    close_handles();
    return false;
}

std::filesystem::path sibling_gui_executable(
    const std::filesystem::path& tray_executable) {
    return tray_executable.parent_path() / "ccs-trans-gui.exe";
}

} // namespace ccs

#endif
