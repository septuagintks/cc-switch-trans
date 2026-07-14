#include "config/config_editing_service.hpp"

#include "config/runtime_compiler.hpp"
#include "protocols/protocol_registry.hpp"
#include "rules/rule_registry.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace ccs {

bool validate_config_candidate(
    const ConfigDocument& document,
    const std::filesystem::path& application_root,
    std::string& error) {
    error.clear();
    if (!validate_config_document(document, error)) {
        return false;
    }

    const auto protocols = builtin_protocol_registry();
    const auto rules = builtin_rule_registry();
    for (const auto& [profile_id, profile] : document.profiles) {
        const bool has_enabled_rules = std::any_of(
            profile.rules.begin(), profile.rules.end(), [](const RuleDefinition& rule) {
                return rule.enabled;
            });
        if (!profile.enabled && !has_enabled_rules) {
            continue;
        }
        if (!profile.protocol) {
            error = "profile " + profile_id
                + " must set protocol before enabling the profile or one of its rules";
            return false;
        }
        const auto handler = protocols->find(profile.protocol->value);
        if (!handler) {
            error = "profile " + profile_id + " uses unknown protocol: "
                + profile.protocol->value;
            return false;
        }
        if (profile.enabled
            && !protocols->validate_profile(handler, profile_id, profile, error)) {
            error = "profile " + profile_id + ": " + error;
            return false;
        }
        std::shared_ptr<const CompiledPipeline> pipeline;
        if (!rules->compile_pipeline(profile.rules, handler, pipeline, error)) {
            error = "profile " + profile_id + ": " + error;
            return false;
        }
    }

    std::filesystem::path resolved_log_path;
    if (!resolve_application_log_path(
            document.application, application_root, resolved_log_path, error)) {
        return false;
    }
    const bool has_enabled_profiles = std::any_of(
        document.profiles.begin(), document.profiles.end(), [](const auto& entry) {
            return entry.second.enabled;
        });
    if (has_enabled_profiles) {
        RuntimeCompiler compiler(application_root);
        RuntimeSnapshotPtr snapshot;
        if (!compiler.compile(document, {}, snapshot, error)) {
            return false;
        }
    }
    return true;
}

ConfigEditingService::ConfigEditingService(ConfigRepository& repository)
    : repository_(repository) {}

bool ConfigEditingService::begin(std::string& error) {
    error.clear();
    if (active_) {
        error = "config editing service already has an active draft";
        return false;
    }
    if (!repository_.loaded()) {
        error = "config repository is not loaded";
        return false;
    }
    draft_ = repository_.document();
    active_ = true;
    return true;
}

bool ConfigEditingService::validate(std::string& error) const {
    if (!active_) {
        error = "config editing service has no active draft";
        return false;
    }
    return validate_config_candidate(draft_, repository_.paths().root, error);
}

bool ConfigEditingService::commit(std::string& error) {
    if (!validate(error) || !repository_.save(draft_, error)) {
        return false;
    }
    active_ = false;
    return true;
}

void ConfigEditingService::discard() noexcept {
    active_ = false;
}

bool ConfigEditingService::active() const noexcept {
    return active_;
}

ConfigDocument& ConfigEditingService::draft() {
    if (!active_) {
        throw std::logic_error("config editing service has no active draft");
    }
    return draft_;
}

const ConfigDocument& ConfigEditingService::draft() const {
    if (!active_) {
        throw std::logic_error("config editing service has no active draft");
    }
    return draft_;
}

} // namespace ccs
