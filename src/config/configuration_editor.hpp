#pragma once

#include "config/composite_config_repository.hpp"
#include "config/field_descriptor.hpp"
#include "config/rules_text.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace ccs {

struct SetConfigurationFieldCommand {
    std::optional<ProfileKey> profile_key;
    std::string key;
    ConfigurationFieldValue value;
};

struct ResetConfigurationFieldCommand {
    std::optional<ProfileKey> profile_key;
    std::string key;
};

class ConfigurationEditor final {
public:
    explicit ConfigurationEditor(CompositeConfigRepository& repository);

    bool begin(std::string& error);
    void discard() noexcept;
    bool active() const noexcept;
    const ConfigurationSnapshot& draft() const;

    bool apply(const SetConfigurationFieldCommand& command, std::string& error);
    bool apply(const ResetConfigurationFieldCommand& command, std::string& error);

    bool create_profile(std::string profile_id, std::string& error);
    bool create_profile(
        std::string profile_id,
        ProfileKey& draft_profile_key,
        std::string& error);
    bool remove_profile(ProfileKey profile_key, std::string& error);
    bool move_profile(ProfileKey profile_key, std::size_t position, std::string& error);

    bool add_rule(
        ProfileKey profile_key,
        std::string rule_id,
        std::string rule_type,
        std::string& error);
    bool remove_rule(
        ProfileKey profile_key,
        std::string_view rule_id,
        std::string& error);
    bool set_rule_enabled(
        ProfileKey profile_key,
        std::string_view rule_id,
        bool enabled,
        std::string& error);
    bool set_rule_option(
        ProfileKey profile_key,
        std::string_view rule_id,
        std::string option,
        std::string_view value_json,
        std::string& error);
    bool unset_rule_option(
        ProfileKey profile_key,
        std::string_view rule_id,
        std::string_view option,
        std::string& error);
    bool move_rule(
        ProfileKey profile_key,
        std::string_view rule_id,
        std::size_t position,
        std::string& error);
    bool replace_rules_text(
        ProfileKey profile_key,
        std::string_view content,
        RulesTextError& parse_error,
        std::string& error);
    bool format_rules_text(
        ProfileKey profile_key,
        std::string& content,
        RulesTextError& format_error,
        std::string& error) const;

    bool validate(std::string& error) const;
    bool commit(ConfigurationSnapshot& committed, std::string& error);

    StoredProfile* find_profile_by_id(std::string_view profile_id);
    const StoredProfile* find_profile_by_id(std::string_view profile_id) const;

private:
    StoredProfile* find_profile(ProfileKey profile_key);
    const StoredProfile* find_profile(ProfileKey profile_key) const;
    bool require_active(std::string& error) const;

    CompositeConfigRepository& repository_;
    ConfigurationSnapshot draft_;
    ProfileKey next_draft_profile_key_ = -1;
    RuleKey next_draft_rule_key_ = -1;
    bool active_ = false;
};

} // namespace ccs
