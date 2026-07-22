#pragma once

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace ccs {

struct GuiPipeIdentity {
    std::wstring current_user_sid;
    std::wstring gui_pipe_name;
    std::wstring maintenance_pipe_name;
    std::string identity_hash;
};

class CurrentUserPipeSecurity {
public:
    CurrentUserPipeSecurity() = default;
    ~CurrentUserPipeSecurity();

    CurrentUserPipeSecurity(const CurrentUserPipeSecurity&) = delete;
    CurrentUserPipeSecurity& operator=(const CurrentUserPipeSecurity&) = delete;

    bool initialize(const std::wstring& user_sid, std::string& error);
    SECURITY_ATTRIBUTES* attributes() noexcept;

private:
    PSECURITY_DESCRIPTOR descriptor_ = nullptr;
    SECURITY_ATTRIBUTES attributes_{};
};

bool current_process_user_sid(std::wstring& sid, std::string& error);
bool process_user_sid(
    std::uint64_t process_id,
    std::wstring& sid,
    std::string& error);
bool make_gui_pipe_identity(
    const std::filesystem::path& config_root,
    std::string_view instance_identity,
    GuiPipeIdentity& identity,
    std::string& error);
bool generate_gui_secret(std::string& secret, std::string& error);

} // namespace ccs

#endif
