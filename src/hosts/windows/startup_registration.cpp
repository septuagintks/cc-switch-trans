#include "hosts/windows/startup_registration.hpp"
#include "hosts/windows/windows_error.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace ccs {

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kValueName[] = L"ccs-trans";

bool validate_executable_path(const std::filesystem::path& executable, std::string& error) {
    if (!executable.is_absolute()) {
        error = "startup executable path must be absolute";
        return false;
    }
    if (executable.native().find(L'\"') != std::wstring::npos) {
        error = "startup executable path contains an invalid quote";
        return false;
    }
    return true;
}

class WindowsStartupValueStore final : public StartupValueStore {
public:
    bool read(std::optional<std::wstring>& value, std::string& error) const override {
        value.reset();
        DWORD bytes = 0;
        auto result = RegGetValueW(
            HKEY_CURRENT_USER,
            kRunKey,
            kValueName,
            RRF_RT_REG_SZ,
            nullptr,
            nullptr,
            &bytes);
        if (result == ERROR_FILE_NOT_FOUND) {
            return true;
        }
        if (result != ERROR_SUCCESS) {
            error = windows_error_message("failed to read startup registration", result);
            return false;
        }

        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        result = RegGetValueW(
            HKEY_CURRENT_USER,
            kRunKey,
            kValueName,
            RRF_RT_REG_SZ,
            nullptr,
            buffer.data(),
            &bytes);
        if (result != ERROR_SUCCESS) {
            error = windows_error_message("failed to read startup registration", result);
            return false;
        }
        value = std::wstring(buffer.data());
        return true;
    }

    bool write(const std::optional<std::wstring>& value, std::string& error) const override {
        HKEY key = nullptr;
        auto result = RegCreateKeyExW(
            HKEY_CURRENT_USER,
            kRunKey,
            0,
            nullptr,
            0,
            KEY_QUERY_VALUE | KEY_SET_VALUE,
            nullptr,
            &key,
            nullptr);
        if (result != ERROR_SUCCESS) {
            error = windows_error_message("failed to open startup registration", result);
            return false;
        }

        if (value) {
            result = RegSetValueExW(
                key,
                kValueName,
                0,
                REG_SZ,
                reinterpret_cast<const BYTE*>(value->c_str()),
                static_cast<DWORD>((value->size() + 1) * sizeof(wchar_t)));
        } else {
            result = RegDeleteValueW(key, kValueName);
            if (result == ERROR_FILE_NOT_FOUND) {
                result = ERROR_SUCCESS;
            }
        }
        RegCloseKey(key);

        if (result != ERROR_SUCCESS) {
            error = windows_error_message("failed to update startup registration", result);
            return false;
        }
        return true;
    }
};

} // namespace

std::wstring windows_startup_command(const std::filesystem::path& executable) {
    return L"\"" + executable.lexically_normal().native() + L"\"";
}

bool windows_startup_command_matches(
    const std::wstring& command,
    const std::filesystem::path& executable) {
    const auto expected = windows_startup_command(executable);
    return CompareStringOrdinal(
        command.data(),
        static_cast<int>(command.size()),
        expected.data(),
        static_cast<int>(expected.size()),
        TRUE) == CSTR_EQUAL;
}

WindowsStartupRegistration::WindowsStartupRegistration(
    std::shared_ptr<StartupValueStore> store)
    : store_(store ? std::move(store) : std::make_shared<WindowsStartupValueStore>()) {}

bool WindowsStartupRegistration::registered(
    const std::filesystem::path& executable,
    bool& enabled,
    std::string& error) const {
    error.clear();
    enabled = false;
    if (!validate_executable_path(executable, error)) {
        return false;
    }

    std::optional<std::wstring> command;
    if (!store_->read(command, error)) {
        return false;
    }
    enabled = command && windows_startup_command_matches(*command, executable);
    return true;
}

bool WindowsStartupRegistration::set_registered(
    const std::filesystem::path& executable,
    bool enabled,
    std::string& error) const {
    error.clear();
    if (!validate_executable_path(executable, error)) {
        return false;
    }

    std::optional<std::wstring> current;
    if (!store_->read(current, error)) {
        return false;
    }
    if (enabled) {
        if (current && windows_startup_command_matches(*current, executable)) {
            return true;
        }
        return store_->write(windows_startup_command(executable), error);
    }
    if (!current) {
        return true;
    }
    return store_->write(std::nullopt, error);
}

} // namespace ccs

#endif
