#include "config/profile_store.hpp"

#include <nlohmann/json.hpp>

#include <charconv>
#include <chrono>
#include <fstream>
#include <iterator>
#include <sstream>
#include <system_error>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

namespace ccs {

namespace {

constexpr const char* kSchemaVersion = "ccs-trans.config/v1";

std::string value_to_string(const ProfileValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean ? "true" : "false";
    }
    return std::to_string(std::get<std::uint64_t>(value));
}

nlohmann::json value_to_json(const ProfileValue& value) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    if (const auto* boolean = std::get_if<bool>(&value)) {
        return *boolean;
    }
    return std::get<std::uint64_t>(value);
}

bool parse_cli_value(
    const std::string& key,
    const std::string& raw,
    ProfileValue& value,
    std::string& error) {
    const auto type = config_value_type(key);
    if (!type) {
        error = "unknown config key: " + key;
        return false;
    }
    if (*type == ConfigValueType::String) {
        value = raw;
        return true;
    }
    if (*type == ConfigValueType::Boolean) {
        if (raw == "true") {
            value = true;
            return true;
        }
        if (raw == "false") {
            value = false;
            return true;
        }
        error = key + " must be true or false";
        return false;
    }

    std::uint64_t parsed = 0;
    const auto result = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (raw.empty() || raw.front() == '-' || result.ec != std::errc{} || result.ptr != raw.data() + raw.size()) {
        error = key + " must be a non-negative integer";
        return false;
    }
    value = parsed;
    return true;
}

bool json_to_profile_value(
    const std::string& key,
    const nlohmann::json& source,
    ProfileValue& value,
    std::string& error) {
    const auto type = config_value_type(key);
    if (!type) {
        error = "unknown config key: " + key;
        return false;
    }
    if (*type == ConfigValueType::String) {
        if (!source.is_string()) {
            error = "profile key " + key + " must be a JSON string";
            return false;
        }
        value = source.get<std::string>();
        return true;
    }
    if (*type == ConfigValueType::Boolean) {
        if (!source.is_boolean()) {
            error = "profile key " + key + " must be a JSON boolean";
            return false;
        }
        value = source.get<bool>();
        return true;
    }
    if (!source.is_number_unsigned() && !source.is_number_integer()) {
        error = "profile key " + key + " must be a non-negative JSON integer";
        return false;
    }
    if (source.is_number_integer() && source.get<std::int64_t>() < 0) {
        error = "profile key " + key + " must be non-negative";
        return false;
    }
    value = source.get<std::uint64_t>();
    return true;
}

nlohmann::json document_json(
    const std::map<std::string, ProfileValues>& profiles,
    const std::optional<std::string>& active_profile) {
    nlohmann::json root = nlohmann::json::object();
    root["schema_version"] = kSchemaVersion;
    root["active_profile"] = active_profile ? nlohmann::json(*active_profile) : nlohmann::json(nullptr);
    root["profiles"] = nlohmann::json::object();
    for (const auto& [name, values] : profiles) {
        auto profile = nlohmann::json::object();
        for (const auto& [key, value] : values) {
            profile[key] = value_to_json(value);
        }
        root["profiles"][name] = std::move(profile);
    }
    return root;
}

std::filesystem::path temporary_path(const std::filesystem::path& target) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
#ifdef _WIN32
    const auto process_id = static_cast<unsigned long long>(GetCurrentProcessId());
#else
    const auto process_id = static_cast<unsigned long long>(getpid());
#endif
    return target.parent_path()
        / (target.filename().string() + ".tmp-" + std::to_string(process_id) + "-" + std::to_string(nonce));
}

bool replace_file(
    const std::filesystem::path& source,
    const std::filesystem::path& target,
    std::string& error) {
#ifdef _WIN32
    if (!MoveFileExW(
            source.c_str(),
            target.c_str(),
            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = "failed to atomically replace config file: Windows error "
            + std::to_string(GetLastError());
        return false;
    }
#else
    if (std::rename(source.c_str(), target.c_str()) != 0) {
        error = "failed to atomically replace config file";
        return false;
    }
#endif
    return true;
}

bool parse_document(
    const nlohmann::json& root,
    std::map<std::string, ProfileValues>& profiles,
    std::optional<std::string>& active_profile,
    std::string& error) {
    if (!root.is_object()
        || root.size() != 3
        || !root.contains("schema_version")
        || !root.contains("active_profile")
        || !root.contains("profiles")) {
        error = "config must contain only schema_version, active_profile, and profiles";
        return false;
    }
    if (!root["schema_version"].is_string()
        || root["schema_version"].get<std::string>() != kSchemaVersion) {
        error = "unsupported config schema_version";
        return false;
    }
    if (!root["profiles"].is_object()) {
        error = "config profiles must be a JSON object";
        return false;
    }

    profiles.clear();
    for (auto profile_it = root["profiles"].begin(); profile_it != root["profiles"].end(); ++profile_it) {
        if (!is_valid_profile_name(profile_it.key())) {
            error = "invalid profile name in config: " + profile_it.key();
            return false;
        }
        if (!profile_it.value().is_object()) {
            error = "profile " + profile_it.key() + " must be a JSON object";
            return false;
        }
        ProfileValues values;
        for (auto value_it = profile_it.value().begin(); value_it != profile_it.value().end(); ++value_it) {
            ProfileValue value;
            if (!json_to_profile_value(value_it.key(), value_it.value(), value, error)) {
                error = "profile " + profile_it.key() + ": " + error;
                return false;
            }
            values.emplace(value_it.key(), std::move(value));
        }
        profiles.emplace(profile_it.key(), std::move(values));
    }

    if (root["active_profile"].is_null()) {
        active_profile.reset();
    } else if (root["active_profile"].is_string()) {
        active_profile = root["active_profile"].get<std::string>();
        if (!is_valid_profile_name(*active_profile) || profiles.count(*active_profile) == 0) {
            error = "active_profile must name an existing profile";
            return false;
        }
    } else {
        error = "active_profile must be a string or null";
        return false;
    }
    return true;
}

} // namespace

ProfileStore::ProfileStore(AppPaths paths)
    : paths_(std::move(paths)) {}

bool ProfileStore::load(std::string& error) {
    loaded_ = false;
    profiles_.clear();
    active_profile_.reset();
    std::error_code exists_error;
    const bool exists = std::filesystem::exists(paths_.config_file, exists_error);
    if (exists_error) {
        error = "failed to inspect config file: " + exists_error.message();
        return false;
    }
    if (!exists) {
        loaded_ = true;
        return true;
    }

    std::ifstream input(paths_.config_file, std::ios::binary);
    if (!input) {
        error = "failed to open config file: " + paths_.config_file.string();
        return false;
    }
    const std::string content{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
    try {
        const auto root = nlohmann::json::parse(content);
        if (!parse_document(root, profiles_, active_profile_, error)) {
            return false;
        }
    } catch (const nlohmann::json::exception& ex) {
        error = "failed to parse config file: " + std::string(ex.what());
        return false;
    }
    if (!validate_document(error)) {
        return false;
    }
    loaded_ = true;
    return true;
}

bool ProfileStore::save(std::string& error) const {
    if (!loaded_) {
        error = "profile store must be loaded before saving";
        return false;
    }
    if (!validate_document(error) || !ensure_app_directories(paths_, error)) {
        return false;
    }

    const auto serialized = document_json(profiles_, active_profile_).dump(2) + "\n";
    const auto temp = temporary_path(paths_.config_file);
    const auto remove_temp = [&]() {
        std::error_code ec;
        std::filesystem::remove(temp, ec);
    };
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "failed to create temporary config file: " + temp.string();
            return false;
        }
        output.write(serialized.data(), static_cast<std::streamsize>(serialized.size()));
        output.flush();
        if (!output) {
            error = "failed to write temporary config file: " + temp.string();
            remove_temp();
            return false;
        }
    }
#ifndef _WIN32
    std::error_code permissions_error;
    std::filesystem::permissions(
        temp,
        std::filesystem::perms::owner_read | std::filesystem::perms::owner_write,
        std::filesystem::perm_options::replace,
        permissions_error);
    if (permissions_error) {
        error = "failed to restrict temporary config file: " + permissions_error.message();
        remove_temp();
        return false;
    }
#endif

    try {
        std::ifstream verification_input(temp, std::ios::binary);
        const std::string verification_content{
            std::istreambuf_iterator<char>(verification_input),
            std::istreambuf_iterator<char>()};
        std::map<std::string, ProfileValues> verified_profiles;
        std::optional<std::string> verified_active;
        const auto verified_json = nlohmann::json::parse(verification_content);
        if (!parse_document(verified_json, verified_profiles, verified_active, error)) {
            remove_temp();
            return false;
        }
    } catch (const nlohmann::json::exception& ex) {
        error = "failed to verify temporary config file: " + std::string(ex.what());
        remove_temp();
        return false;
    }

    if (!replace_file(temp, paths_.config_file, error)) {
        remove_temp();
        return false;
    }
    return true;
}

std::vector<std::string> ProfileStore::profile_names() const {
    std::vector<std::string> names;
    names.reserve(profiles_.size());
    for (const auto& [name, values] : profiles_) {
        (void)values;
        names.push_back(name);
    }
    return names;
}

std::optional<std::string> ProfileStore::active_profile() const {
    return active_profile_;
}

std::string ProfileStore::list_json() const {
    nlohmann::json output = nlohmann::json::object();
    output["active_profile"] = active_profile_ ? nlohmann::json(*active_profile_) : nlohmann::json(nullptr);
    output["profiles"] = profile_names();
    return output.dump(2) + "\n";
}

bool ProfileStore::show_json(const std::string& name, std::string& output, std::string& error) const {
    const auto profile = profiles_.find(name);
    if (profile == profiles_.end()) {
        error = "profile does not exist: " + name;
        return false;
    }
    nlohmann::json rendered = nlohmann::json::object();
    rendered["name"] = name;
    rendered["active"] = active_profile_ && *active_profile_ == name;
    rendered["values"] = nlohmann::json::object();
    for (const auto& [key, value] : profile->second) {
        rendered["values"][key] = value_to_json(value);
    }
    output = rendered.dump(2) + "\n";
    return true;
}

bool ProfileStore::create(const std::string& name, std::string& error) {
    if (!is_valid_profile_name(name)) {
        error = "invalid profile name: " + name;
        return false;
    }
    if (profiles_.count(name) != 0) {
        error = "profile already exists: " + name;
        return false;
    }
    profiles_.emplace(name, ProfileValues{});
    return true;
}

bool ProfileStore::remove(const std::string& name, std::string& error) {
    if (profiles_.erase(name) == 0) {
        error = "profile does not exist: " + name;
        return false;
    }
    if (active_profile_ && *active_profile_ == name) {
        active_profile_.reset();
    }
    return true;
}

bool ProfileStore::use(const std::string& name, std::string& error) {
    if (profiles_.count(name) == 0) {
        error = "profile does not exist: " + name;
        return false;
    }
    active_profile_ = name;
    return true;
}

bool ProfileStore::set(
    const std::string& name,
    const std::string& key,
    const std::string& value,
    std::string& error) {
    const auto profile = profiles_.find(name);
    if (profile == profiles_.end()) {
        error = "profile does not exist: " + name;
        return false;
    }
    ProfileValue parsed;
    if (!parse_cli_value(key, value, parsed, error)) {
        return false;
    }
    auto candidate = profile->second;
    candidate[key] = std::move(parsed);
    if (!validate_profile(candidate, error)) {
        return false;
    }
    profile->second = std::move(candidate);
    return true;
}

bool ProfileStore::unset(const std::string& name, const std::string& key, std::string& error) {
    const auto profile = profiles_.find(name);
    if (profile == profiles_.end()) {
        error = "profile does not exist: " + name;
        return false;
    }
    if (profile->second.count(key) == 0) {
        error = "profile key is not set: " + key;
        return false;
    }
    auto candidate = profile->second;
    candidate.erase(key);
    if (!validate_profile(candidate, error)) {
        return false;
    }
    profile->second = std::move(candidate);
    return true;
}

bool ProfileStore::resolve_run(
    const ParseResult& command,
    ConfigSnapshot& snapshot,
    std::string& selected_profile,
    std::string& error) const {
    if (command.command != CliCommandKind::Run) {
        error = "only run commands can resolve a runtime configuration";
        return false;
    }
    AppConfig config;
    config.log_path = paths_.default_log_file;

    std::optional<std::string> profile_name;
    if (!command.profile_name.empty()) {
        profile_name = command.profile_name;
    } else {
        profile_name = active_profile_;
    }
    if (profile_name) {
        const auto profile = profiles_.find(*profile_name);
        if (profile == profiles_.end()) {
            error = "profile does not exist: " + *profile_name;
            return false;
        }
        if (!apply_profile(profile->second, config, error)) {
            return false;
        }
        selected_profile = *profile_name;
    } else {
        selected_profile.clear();
    }

    for (const auto& override_value : command.overrides) {
        if (!apply_config_override(config, override_value.key, override_value.value, error)) {
            return false;
        }
    }
    if (config.log_path.is_relative()) {
        const auto root = paths_.root.lexically_normal();
        const auto resolved = (root / config.log_path).lexically_normal();
        const auto relative = resolved.lexically_relative(root);
        if (relative.empty() || relative == "." || *relative.begin() == "..") {
            error = "relative log path must stay within the application root";
            return false;
        }
        config.log_path = resolved;
    }
    if (!validate_config(config, error)) {
        return false;
    }
    snapshot = make_config_snapshot(std::move(config));
    return true;
}

const AppPaths& ProfileStore::paths() const {
    return paths_;
}

bool ProfileStore::validate_document(std::string& error) const {
    if (active_profile_ && profiles_.count(*active_profile_) == 0) {
        error = "active profile does not exist: " + *active_profile_;
        return false;
    }
    for (const auto& [name, values] : profiles_) {
        if (!is_valid_profile_name(name)) {
            error = "invalid profile name: " + name;
            return false;
        }
        if (!validate_profile(values, error)) {
            error = "profile " + name + ": " + error;
            return false;
        }
    }
    return true;
}

bool ProfileStore::apply_profile(const ProfileValues& values, AppConfig& config, std::string& error) const {
    for (const auto& [key, value] : values) {
        if (!apply_config_override(config, key, value_to_string(value), error)) {
            return false;
        }
    }
    return true;
}

bool ProfileStore::validate_profile(const ProfileValues& values, std::string& error) const {
    AppConfig config;
    config.log_path = paths_.default_log_file;
    if (!apply_profile(values, config, error)) {
        return false;
    }
    if (config.log_path.is_relative()) {
        const auto root = paths_.root.lexically_normal();
        const auto resolved = (root / config.log_path).lexically_normal();
        const auto relative = resolved.lexically_relative(root);
        if (relative.empty() || relative == "." || *relative.begin() == "..") {
            error = "relative log path must stay within the application root";
            return false;
        }
        config.log_path = resolved;
    }
    return validate_profile_config(config, error);
}

} // namespace ccs
