#include "hosts/windows/windows_error.hpp"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace ccs {

std::string windows_error_message(const std::string& operation, unsigned long code) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        code,
        0,
        reinterpret_cast<wchar_t*>(&message),
        0,
        nullptr);
    std::string detail;
    if (length != 0 && message != nullptr) {
        const int bytes = WideCharToMultiByte(
            CP_UTF8, 0, message, static_cast<int>(length), nullptr, 0, nullptr, nullptr);
        if (bytes > 0) {
            detail.resize(static_cast<std::size_t>(bytes));
            (void)WideCharToMultiByte(
                CP_UTF8,
                0,
                message,
                static_cast<int>(length),
                detail.data(),
                bytes,
                nullptr,
                nullptr);
            while (!detail.empty()
                && (detail.back() == '\r' || detail.back() == '\n' || detail.back() == ' ')) {
                detail.pop_back();
            }
        }
        LocalFree(message);
    }
    if (detail.empty()) {
        detail = "Windows error " + std::to_string(code);
    }
    return operation + ": " + detail;
}

} // namespace ccs

#endif
