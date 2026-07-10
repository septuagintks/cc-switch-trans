#pragma once

#include "config/app_paths.hpp"
#include "config/config.hpp"

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace ccs {

using ProfileValue = std::variant<std::string, bool, std::uint64_t>;
using ProfileValues = std::map<std::string, ProfileValue>;

class ProfileStore {
public:
    explicit ProfileStore(AppPaths paths);

    bool load(std::string& error);
    bool save(std::string& error) const;

    std::vector<std::string> profile_names() const;
    std::optional<std::string> active_profile() const;
    std::string list_json() const;
    bool show_json(const std::string& name, std::string& output, std::string& error) const;

    bool create(const std::string& name, std::string& error);
    bool remove(const std::string& name, std::string& error);
    bool use(const std::string& name, std::string& error);
    bool set(const std::string& name, const std::string& key, const std::string& value, std::string& error);
    bool unset(const std::string& name, const std::string& key, std::string& error);

    bool resolve_run(
        const ParseResult& command,
        ConfigSnapshot& snapshot,
        std::string& selected_profile,
        std::string& error) const;

    const AppPaths& paths() const;

private:
    bool validate_document(std::string& error) const;
    bool apply_profile(const ProfileValues& values, AppConfig& config, std::string& error) const;
    bool validate_profile(const ProfileValues& values, std::string& error) const;

    AppPaths paths_;
    std::map<std::string, ProfileValues> profiles_;
    std::optional<std::string> active_profile_;
    bool loaded_ = false;
};

} // namespace ccs
