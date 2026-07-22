#include "hosts/windows/platform/gui_pipe_security.hpp"

#ifdef _WIN32

#include "core/sha256.hpp"
#include "hosts/windows/windows_error.hpp"

#include <bcrypt.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cwctype>
#include <iomanip>
#include <limits>
#include <sstream>
#include <system_error>
#include <vector>

namespace ccs {

namespace {

bool token_user_sid(HANDLE token, std::wstring& sid, std::string& error) {
    DWORD required = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0 || GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        error = windows_error_message(
            "failed to size the Windows token user", GetLastError());
        return false;
    }
    std::vector<std::uint8_t> buffer(required);
    if (!GetTokenInformation(
            token, TokenUser, buffer.data(), required, &required)) {
        error = windows_error_message(
            "failed to read the Windows token user", GetLastError());
        return false;
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    LPWSTR text = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &text)) {
        error = windows_error_message(
            "failed to format the Windows user SID", GetLastError());
        return false;
    }
    sid = text;
    LocalFree(text);
    return true;
}

bool wide_to_utf8(std::wstring_view value, std::string& converted, std::string& error) {
    converted.clear();
    if (value.empty()) return true;
    const int required = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        error = windows_error_message(
            "failed to encode a Windows path as UTF-8", GetLastError());
        return false;
    }
    converted.resize(static_cast<std::size_t>(required));
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
            converted.data(), required, nullptr, nullptr) != required) {
        error = windows_error_message(
            "failed to encode a Windows path as UTF-8", GetLastError());
        return false;
    }
    return true;
}

std::filesystem::path canonical_identity_path(
    const std::filesystem::path& path,
    std::error_code& error) {
    auto absolute = std::filesystem::absolute(path, error);
    if (error) return {};
    auto canonical = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        error.clear();
        canonical = absolute.lexically_normal();
    }
    return canonical;
}

} // namespace

CurrentUserPipeSecurity::~CurrentUserPipeSecurity() {
    if (descriptor_ != nullptr) LocalFree(descriptor_);
}

bool CurrentUserPipeSecurity::initialize(
    const std::wstring& user_sid,
    std::string& error) {
    error.clear();
    if (descriptor_ != nullptr) {
        error = "named-pipe security was already initialized";
        return false;
    }
    const std::wstring sddl = L"D:P(A;;GA;;;" + user_sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            sddl.c_str(), SDDL_REVISION_1, &descriptor_, nullptr)) {
        error = windows_error_message(
            "failed to build the current-user named-pipe DACL", GetLastError());
        return false;
    }
    attributes_.nLength = sizeof(attributes_);
    attributes_.lpSecurityDescriptor = descriptor_;
    attributes_.bInheritHandle = FALSE;
    return true;
}

SECURITY_ATTRIBUTES* CurrentUserPipeSecurity::attributes() noexcept {
    return descriptor_ == nullptr ? nullptr : &attributes_;
}

bool current_process_user_sid(std::wstring& sid, std::string& error) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        error = windows_error_message(
            "failed to open the current Windows process token", GetLastError());
        return false;
    }
    const bool succeeded = token_user_sid(token, sid, error);
    CloseHandle(token);
    return succeeded;
}

bool process_user_sid(
    std::uint64_t process_id,
    std::wstring& sid,
    std::string& error) {
    if (process_id == 0 || process_id > std::numeric_limits<DWORD>::max()) {
        error = "named-pipe client process id is outside the Windows range";
        return false;
    }
    HANDLE process = OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(process_id));
    if (process == nullptr) {
        error = windows_error_message(
            "failed to open the named-pipe client process", GetLastError());
        return false;
    }
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) {
        error = windows_error_message(
            "failed to open the named-pipe client process token", GetLastError());
        CloseHandle(process);
        return false;
    }
    const bool succeeded = token_user_sid(token, sid, error);
    CloseHandle(token);
    CloseHandle(process);
    return succeeded;
}

bool make_gui_pipe_identity(
    const std::filesystem::path& config_root,
    std::string_view instance_identity,
    GuiPipeIdentity& identity,
    std::string& error) {
    error.clear();
    if (instance_identity.empty()) {
        error = "GUI instance identity must not be empty";
        return false;
    }
    GuiPipeIdentity built;
    if (!current_process_user_sid(built.current_user_sid, error)) return false;
    std::error_code path_error;
    auto canonical = canonical_identity_path(config_root, path_error);
    if (path_error) {
        error = "failed to canonicalize the GUI config root: " + path_error.message();
        return false;
    }
    auto native = canonical.native();
    std::transform(native.begin(), native.end(), native.begin(), [](wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    std::string sid_utf8;
    std::string path_utf8;
    if (!wide_to_utf8(built.current_user_sid, sid_utf8, error)
        || !wide_to_utf8(native, path_utf8, error)) {
        return false;
    }
    const std::string material = "ccs-trans.gui-ipc/v1\n" + sid_utf8 + "\n"
        + path_utf8 + "\n" + std::string(instance_identity);
    built.identity_hash = sha256_hex(material);
    const auto public_hash = built.identity_hash.substr(0, 32);
    built.gui_pipe_name = L"\\\\.\\pipe\\ccs-trans.gui-ipc.v1."
        + std::wstring(public_hash.begin(), public_hash.end());
    built.maintenance_pipe_name = L"\\\\.\\pipe\\ccs-trans.maintenance.v1."
        + std::wstring(public_hash.begin(), public_hash.end());
    identity = std::move(built);
    return true;
}

bool generate_gui_secret(std::string& secret, std::string& error) {
    std::array<std::uint8_t, 32> bytes{};
    const auto status = BCryptGenRandom(
        nullptr,
        bytes.data(),
        static_cast<ULONG>(bytes.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status < 0) {
        error = "Windows cryptographic random generation failed";
        return false;
    }
    std::ostringstream formatted;
    formatted << std::hex << std::setfill('0');
    for (const auto byte : bytes) formatted << std::setw(2) << static_cast<unsigned int>(byte);
    secret = formatted.str();
    error.clear();
    return true;
}

} // namespace ccs

#endif
