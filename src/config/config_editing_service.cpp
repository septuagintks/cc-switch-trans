#include "config/config_editing_service.hpp"

#include "config/runtime_compiler.hpp"
#include "core/url.hpp"
#include "protocols/protocol_registry.hpp"
#include "rules/rule_registry.hpp"

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <unordered_map>

namespace ccs {

namespace {

struct CandidateRoute {
    std::string profile_id;
    std::string field;
};

bool add_candidate_route(
    std::unordered_map<std::string, CandidateRoute>& routes,
    std::string_view method,
    std::string_view path,
    const std::string& profile_id,
    std::string field,
    ConfigValidationFailure& failure) {
    std::string canonical;
    std::string error;
    if (!canonicalize_http_path(std::string(path), canonical, error)) {
        failure = {
            ConfigValidationError::InvalidProfile,
            profile_id,
            std::move(field),
            std::move(error),
        };
        return false;
    }
    std::string key(method);
    key.push_back('\0');
    key.append(canonical);
    const auto [existing, inserted] = routes.emplace(
        std::move(key), CandidateRoute{profile_id, field});
    if (inserted) {
        return true;
    }
    const auto detail = "route collision for " + std::string(method) + " " + canonical
        + " between profiles " + existing->second.profile_id + " and " + profile_id;
    failure = {
        ConfigValidationError::RouteCollision,
        profile_id,
        std::move(field),
        detail,
    };
    return false;
}

} // namespace

bool validate_config_candidate(
    const ConfigDocument& document,
    const std::filesystem::path& application_root,
    std::string& error) {
    ConfigValidationFailure failure;
    const bool valid = validate_config_candidate(document, application_root, failure);
    error = std::move(failure.detail);
    return valid;
}

bool validate_config_candidate(
    const ConfigDocument& document,
    const std::filesystem::path& application_root,
    ConfigValidationFailure& failure) {
    failure = {};
    std::string error;
    ConfigDocumentValidationFailure document_failure;
    if (!validate_config_document(document, document_failure)) {
        failure = {
            ConfigValidationError::InvalidDocument,
            std::move(document_failure.profile_id),
            std::move(document_failure.field),
            std::move(document_failure.detail),
        };
        return false;
    }

    const auto protocols = builtin_protocol_registry();
    const auto rules = builtin_rule_registry();
    std::unordered_map<std::string, CandidateRoute> routes;
    for (const auto& [profile_id, profile] : document.profiles) {
        const bool has_enabled_rules = std::any_of(
            profile.rules.begin(), profile.rules.end(), [](const RuleDefinition& rule) {
                return rule.enabled;
            });
        if (!profile.enabled && !has_enabled_rules) {
            continue;
        }
        if (!profile.protocol) {
            failure = {
                ConfigValidationError::InvalidProfile,
                profile_id,
                "protocol",
                "profile " + profile_id
                    + " must set protocol before enabling the profile or one of its rules",
            };
            return false;
        }
        const auto handler = protocols->find(profile.protocol->value);
        if (!handler) {
            failure = {
                ConfigValidationError::UnknownProtocol,
                profile_id,
                "protocol",
                "profile " + profile_id + " uses unknown protocol: "
                    + profile.protocol->value,
            };
            return false;
        }
        if (profile.enabled
            && !protocols->validate_profile(handler, profile_id, profile, error)) {
            failure = {
                ConfigValidationError::InvalidProfile,
                profile_id,
                {},
                "profile " + profile_id + ": " + error,
            };
            return false;
        }
        std::shared_ptr<const CompiledPipeline> pipeline;
        if (!rules->compile_pipeline(profile.rules, handler, pipeline, error)) {
            failure = {
                ConfigValidationError::InvalidRules,
                profile_id,
                "rules",
                "profile " + profile_id + ": " + error,
            };
            return false;
        }
        if (profile.enabled) {
            if (!add_candidate_route(
                    routes,
                    handler->descriptor().request_method,
                    *profile.local.request_path,
                    profile_id,
                    "local.request-path",
                    failure)) {
                return false;
            }
            if (profile.local.usage_path
                && !add_candidate_route(
                    routes,
                    handler->descriptor().usage_method,
                    *profile.local.usage_path,
                    profile_id,
                    "local.usage-path",
                    failure)) {
                return false;
            }
        }
    }

    std::filesystem::path resolved_log_path;
    if (!resolve_application_log_path(
            document.application, application_root, resolved_log_path, error)) {
        failure = {
            ConfigValidationError::InvalidLogPath,
            {},
            "logging.path",
            std::move(error),
        };
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
            failure = {
                ConfigValidationError::RuntimeCompileFailed,
                {},
                {},
                std::move(error),
            };
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
