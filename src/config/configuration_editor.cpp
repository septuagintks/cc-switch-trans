#include "config/configuration_editor.hpp"

#include "config/config_editing_service.hpp"
#include "config/configuration_conversion.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ccs {

namespace {

auto find_rule(StoredProfile& profile, std::string_view rule_id) {
    return std::find_if(profile.rules.begin(), profile.rules.end(), [rule_id](const auto& rule) {
        return rule.rule_id == rule_id;
    });
}

} // namespace

ConfigurationEditor::ConfigurationEditor(ConfigurationRepository& repository)
    : repository_(repository) {}

bool ConfigurationEditor::begin(std::string& error) {
    error.clear();
    clear_failure();
    if (active_) {
        error = "configuration editor already has an active draft";
        return false;
    }
    if (!repository_.loaded() && !repository_.load(error)) {
        return false;
    }
    draft_ = repository_.snapshot();
    next_draft_profile_key_ = -1;
    next_draft_rule_key_ = -1;
    active_ = true;
    return true;
}

void ConfigurationEditor::discard() noexcept {
    active_ = false;
}

bool ConfigurationEditor::active() const noexcept {
    return active_;
}

const ConfigurationSnapshot& ConfigurationEditor::draft() const {
    if (!active_) {
        throw std::logic_error("configuration editor has no active draft");
    }
    return draft_;
}

bool ConfigurationEditor::require_active(std::string& error) const {
    if (active_) {
        return true;
    }
    error = "configuration editor has no active draft";
    set_failure(ConfigurationEditError::Inactive, {}, error);
    return false;
}

void ConfigurationEditor::clear_failure() const {
    last_failure_ = {};
}

void ConfigurationEditor::set_failure(
    ConfigurationEditError code,
    std::string field,
    std::string detail) const {
    last_failure_ = {code, std::move(field), std::move(detail)};
}

StoredProfile* ConfigurationEditor::find_profile(ProfileKey profile_key) {
    const auto found = std::find_if(
        draft_.profiles.begin(), draft_.profiles.end(), [profile_key](const auto& profile) {
            return profile.key == profile_key;
        });
    return found == draft_.profiles.end() ? nullptr : &*found;
}

const StoredProfile* ConfigurationEditor::find_profile(ProfileKey profile_key) const {
    const auto found = std::find_if(
        draft_.profiles.begin(), draft_.profiles.end(), [profile_key](const auto& profile) {
            return profile.key == profile_key;
        });
    return found == draft_.profiles.end() ? nullptr : &*found;
}

StoredProfile* ConfigurationEditor::find_profile_by_id(std::string_view profile_id) {
    const auto found = std::find_if(
        draft_.profiles.begin(), draft_.profiles.end(), [profile_id](const auto& profile) {
            return profile.profile_id == profile_id;
        });
    return found == draft_.profiles.end() ? nullptr : &*found;
}

const StoredProfile* ConfigurationEditor::find_profile_by_id(std::string_view profile_id) const {
    const auto found = std::find_if(
        draft_.profiles.begin(), draft_.profiles.end(), [profile_id](const auto& profile) {
            return profile.profile_id == profile_id;
        });
    return found == draft_.profiles.end() ? nullptr : &*found;
}

bool ConfigurationEditor::apply(
    const SetConfigurationFieldCommand& command,
    std::string& error) {
    error.clear();
    clear_failure();
    if (!require_active(error)) {
        return false;
    }
    const auto scope = command.profile_key
        ? ConfigurationFieldScope::Profile
        : ConfigurationFieldScope::Application;
    const auto* descriptor = find_configuration_field_descriptor(scope, command.key);
    if (descriptor == nullptr) {
        error = "unknown configuration field: " + command.key;
        set_failure(ConfigurationEditError::InvalidField, command.key, error);
        return false;
    }
    if (!command.profile_key) {
        if (!apply_application_field(draft_.application, *descriptor, command.value, error)) {
            set_failure(ConfigurationEditError::InvalidField, command.key, error);
            return false;
        }
        return true;
    }
    auto* profile = find_profile(*command.profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        set_failure(ConfigurationEditError::ProfileNotFound, command.key, error);
        return false;
    }
    if (descriptor->key == "id") {
        if (const auto* id = std::get_if<std::string>(&command.value)) {
            const auto* collision = find_profile_by_id(*id);
            if (collision != nullptr && collision->key != profile->key) {
                error = "profile already exists: " + *id;
                set_failure(ConfigurationEditError::ProfileAlreadyExists, "id", error);
                return false;
            }
        }
    }
    if (!apply_profile_field(*profile, *descriptor, command.value, error)) {
        set_failure(ConfigurationEditError::InvalidField, command.key, error);
        return false;
    }
    return true;
}

bool ConfigurationEditor::apply(
    const ResetConfigurationFieldCommand& command,
    std::string& error) {
    error.clear();
    clear_failure();
    if (!require_active(error)) {
        return false;
    }
    const auto scope = command.profile_key
        ? ConfigurationFieldScope::Profile
        : ConfigurationFieldScope::Application;
    const auto* descriptor = find_configuration_field_descriptor(scope, command.key);
    if (descriptor == nullptr) {
        error = "unknown configuration field: " + command.key;
        set_failure(ConfigurationEditError::InvalidField, command.key, error);
        return false;
    }
    if (!command.profile_key) {
        if (!reset_application_field(draft_.application, *descriptor, error)) {
            set_failure(ConfigurationEditError::InvalidField, command.key, error);
            return false;
        }
        return true;
    }
    auto* profile = find_profile(*command.profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        set_failure(ConfigurationEditError::ProfileNotFound, command.key, error);
        return false;
    }
    if (!reset_profile_field(*profile, *descriptor, error)) {
        set_failure(ConfigurationEditError::InvalidField, command.key, error);
        return false;
    }
    return true;
}

bool ConfigurationEditor::apply_batch(
    std::span<const SetConfigurationFieldCommand> set_commands,
    std::span<const ResetConfigurationFieldCommand> reset_commands,
    bool validate_candidate,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    auto previous = draft_;
    for (const auto& command : set_commands) {
        if (!apply(command, error)) {
            draft_ = std::move(previous);
            return false;
        }
    }
    for (const auto& command : reset_commands) {
        if (!apply(command, error)) {
            draft_ = std::move(previous);
            return false;
        }
    }
    if (validate_candidate && !validate(error)) {
        draft_ = std::move(previous);
        return false;
    }
    return true;
}

bool ConfigurationEditor::create_profile(std::string profile_id, std::string& error) {
    ProfileKey ignored = 0;
    return create_profile(std::move(profile_id), ignored, error);
}

bool ConfigurationEditor::create_profile(
    std::string profile_id,
    ProfileKey& draft_profile_key,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    if (!is_valid_profile_id(profile_id)) {
        error = "profile id must be 1-64 characters using letters, digits, ., _, or -";
        return false;
    }
    if (find_profile_by_id(profile_id) != nullptr) {
        error = "profile already exists: " + profile_id;
        return false;
    }
    StoredProfile profile;
    profile.key = next_draft_profile_key_--;
    profile.profile_id = std::move(profile_id);
    draft_profile_key = profile.key;
    draft_.profiles.push_back(std::move(profile));
    return true;
}

bool ConfigurationEditor::remove_profile(ProfileKey profile_key, std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    const auto found = std::find_if(
        draft_.profiles.begin(), draft_.profiles.end(), [profile_key](const auto& profile) {
            return profile.key == profile_key;
        });
    if (found == draft_.profiles.end()) {
        error = "profile key no longer exists";
        return false;
    }
    draft_.profiles.erase(found);
    return true;
}

bool ConfigurationEditor::move_profile(
    ProfileKey profile_key,
    std::size_t position,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    if (position == 0 || position > draft_.profiles.size()) {
        error = "profile position must be within the current 1-based profile list";
        return false;
    }
    const auto found = std::find_if(
        draft_.profiles.begin(), draft_.profiles.end(), [profile_key](const auto& profile) {
            return profile.key == profile_key;
        });
    if (found == draft_.profiles.end()) {
        error = "profile key no longer exists";
        return false;
    }
    auto moving = std::move(*found);
    draft_.profiles.erase(found);
    draft_.profiles.insert(
        draft_.profiles.begin() + static_cast<std::ptrdiff_t>(position - 1),
        std::move(moving));
    return true;
}

bool ConfigurationEditor::add_rule(
    ProfileKey profile_key,
    std::string rule_id,
    std::string rule_type,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        return false;
    }
    if (!is_valid_rule_id(rule_id) || !is_valid_rule_type(rule_type)) {
        error = "rule id or type is invalid";
        return false;
    }
    if (profile->rules.size() >= kMaxRulesPerProfile) {
        error = "profile exceeds the maximum rule count";
        return false;
    }
    if (find_rule(*profile, rule_id) != profile->rules.end()) {
        error = "rule already exists: " + rule_id;
        return false;
    }
    profile->rules.push_back({
        next_draft_rule_key_--,
        std::move(rule_id),
        false,
        std::move(rule_type),
        "{}",
    });
    return true;
}

bool ConfigurationEditor::remove_rule(
    ProfileKey profile_key,
    std::string_view rule_id,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        return false;
    }
    const auto rule = find_rule(*profile, rule_id);
    if (rule == profile->rules.end()) {
        error = "rule does not exist: " + std::string(rule_id);
        return false;
    }
    profile->rules.erase(rule);
    return true;
}

bool ConfigurationEditor::set_rule_enabled(
    ProfileKey profile_key,
    std::string_view rule_id,
    bool enabled,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        return false;
    }
    const auto rule = find_rule(*profile, rule_id);
    if (rule == profile->rules.end()) {
        error = "rule does not exist: " + std::string(rule_id);
        return false;
    }
    rule->enabled = enabled;
    return true;
}

bool ConfigurationEditor::set_rule_option(
    ProfileKey profile_key,
    std::string_view rule_id,
    std::string option,
    std::string_view value_json,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    if (!is_valid_rule_option_name(option)) {
        error = "rule option name is invalid: " + option;
        return false;
    }
    auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        return false;
    }
    const auto rule = find_rule(*profile, rule_id);
    if (rule == profile->rules.end()) {
        error = "rule does not exist: " + std::string(rule_id);
        return false;
    }
    try {
        auto options = nlohmann::json::parse(rule->options_json);
        auto value = nlohmann::json::parse(value_json);
        if (!options.is_object()) {
            error = "stored rule options are not a JSON object";
            return false;
        }
        options[std::move(option)] = std::move(value);
        auto serialized = options.dump();
        if (serialized.size() > kMaxStoredRuleOptionsBytes) {
            error = "rule options exceed the 1 MiB limit";
            return false;
        }
        rule->options_json = std::move(serialized);
        return true;
    } catch (const nlohmann::json::exception& exception) {
        error = "invalid rule option JSON: " + std::string(exception.what());
        return false;
    }
}

bool ConfigurationEditor::unset_rule_option(
    ProfileKey profile_key,
    std::string_view rule_id,
    std::string_view option,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        return false;
    }
    const auto rule = find_rule(*profile, rule_id);
    if (rule == profile->rules.end()) {
        error = "rule does not exist: " + std::string(rule_id);
        return false;
    }
    try {
        auto options = nlohmann::json::parse(rule->options_json);
        if (!options.is_object() || options.erase(std::string(option)) == 0) {
            error = "rule option is not set: " + std::string(option);
            return false;
        }
        rule->options_json = options.dump();
        return true;
    } catch (const nlohmann::json::exception& exception) {
        error = "invalid stored rule options: " + std::string(exception.what());
        return false;
    }
}

bool ConfigurationEditor::move_rule(
    ProfileKey profile_key,
    std::string_view rule_id,
    std::size_t position,
    std::string& error) {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        return false;
    }
    if (position == 0 || position > profile->rules.size()) {
        error = "rule position must be within the current 1-based rule list";
        return false;
    }
    const auto found = find_rule(*profile, rule_id);
    if (found == profile->rules.end()) {
        error = "rule does not exist: " + std::string(rule_id);
        return false;
    }
    auto moving = std::move(*found);
    profile->rules.erase(found);
    profile->rules.insert(
        profile->rules.begin() + static_cast<std::ptrdiff_t>(position - 1),
        std::move(moving));
    return true;
}

bool ConfigurationEditor::replace_rules_text(
    ProfileKey profile_key,
    std::string_view content,
    RulesTextError& parse_error,
    std::string& error) {
    error.clear();
    clear_failure();
    if (!require_active(error)) {
        return false;
    }
    auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        set_failure(ConfigurationEditError::ProfileNotFound, "rules", error);
        return false;
    }
    std::vector<StoredRule> parsed;
    if (!parse_rules_text(content, profile->rules, parsed, parse_error)) {
        error = parse_error.message;
        set_failure(ConfigurationEditError::RulesInvalid, "rules", error);
        return false;
    }
    for (auto& rule : parsed) {
        if (rule.key == 0) {
            rule.key = next_draft_rule_key_--;
        }
    }
    profile->rules = std::move(parsed);
    return true;
}

bool ConfigurationEditor::format_rules_text(
    ProfileKey profile_key,
    std::string& content,
    RulesTextError& format_error,
    std::string& error) const {
    error.clear();
    if (!require_active(error)) {
        return false;
    }
    const auto* profile = find_profile(profile_key);
    if (profile == nullptr) {
        error = "profile key no longer exists";
        return false;
    }
    if (!ccs::format_rules_text(profile->rules, content, format_error)) {
        error = format_error.message;
        return false;
    }
    return true;
}

bool ConfigurationEditor::validate(std::string& error) const {
    error.clear();
    clear_failure();
    if (!require_active(error)) {
        return false;
    }
    ConfigDocument document;
    ConfigDocumentValidationFailure document_failure;
    if (!configuration_snapshot_to_config_document(
            draft_, document, document_failure)) {
        error = document_failure.detail;
        set_failure(
            ConfigurationEditError::ValidationFailed,
            std::move(document_failure.field),
            error);
        return false;
    }
    ConfigValidationFailure failure;
    if (!validate_config_candidate(document, repository_.paths().root, failure)) {
        error = failure.detail;
        set_failure(
            failure.code == ConfigValidationError::RouteCollision
                ? ConfigurationEditError::RouteCollision
                : ConfigurationEditError::ValidationFailed,
            std::move(failure.field),
            error);
        return false;
    }
    return true;
}

bool ConfigurationEditor::commit(
    ConfigurationSnapshot& committed,
    std::string& error) {
    if (!validate(error)) {
        return false;
    }
    auto desired = draft_;
    for (auto& profile : desired.profiles) {
        if (profile.key < 0) {
            profile.key = 0;
        }
        for (auto& rule : profile.rules) {
            if (rule.key < 0) {
                rule.key = 0;
            }
        }
    }
    if (!repository_.save_snapshot(desired, committed, error)) {
        return false;
    }
    draft_ = committed;
    active_ = false;
    return true;
}

const ConfigurationEditFailure& ConfigurationEditor::last_failure() const noexcept {
    return last_failure_;
}

} // namespace ccs
