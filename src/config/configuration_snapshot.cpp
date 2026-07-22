#include "config/configuration_conversion.hpp"

#include "core/sha256.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>
#include <utility>

namespace ccs {

std::string repository_revision_token(const RepositoryRevision& revision) {
    std::string source;
    source.reserve(revision.application_source.bytes.size() + 96);
    source.append("ccs-trans.repository-revision/v1");
    source.push_back('\0');
    source.push_back(revision.application_source.exists ? '1' : '0');
    source.push_back('\0');
    source.append(std::to_string(revision.profile_revision));
    source.push_back('\0');
    source.append(revision.application_source.bytes);
    return sha256_hex(source);
}

namespace {

bool stored_rule_to_definition(
    const StoredRule& stored,
    RuleDefinition& definition,
    std::string& error) {
    auto options = nlohmann::json::parse(
        stored.options_json.begin(), stored.options_json.end(), nullptr, false, true);
    if (options.is_discarded() || !options.is_object()
        || options.dump() != stored.options_json) {
        error = "rule " + stored.rule_id + " has invalid canonical options_json";
        return false;
    }
    definition.id.value = stored.rule_id;
    definition.enabled = stored.enabled;
    definition.type = stored.type;
    definition.storage_key = stored.key;
    for (auto item = options.begin(); item != options.end(); ++item) {
        definition.options.emplace(item.key(), item.value());
    }
    return true;
}

} // namespace

bool config_document_to_stored_profiles(
    const ConfigDocument& document,
    std::vector<StoredProfile>& profiles,
    std::string& error) {
    error.clear();
    if (!validate_config_document(document, error)) {
        return false;
    }
    std::vector<StoredProfile> converted;
    converted.reserve(document.profiles.size());
    try {
        std::vector<std::pair<std::size_t, StoredProfile>> ordered;
        ordered.reserve(document.profiles.size());
        for (const auto& [profile_id, definition] : document.profiles) {
            StoredProfile profile;
            profile.key = definition.storage_key;
            profile.profile_id = profile_id;
            profile.enabled = definition.enabled;
            if (definition.protocol) {
                profile.protocol = definition.protocol->value;
            }
            profile.local_request_path = definition.local.request_path;
            profile.local_usage_path = definition.local.usage_path;
            profile.upstream_base_url = definition.upstream.base_url;
            profile.upstream_request_path = definition.upstream.request_path;
            profile.upstream_usage_path = definition.upstream.usage_path;
            profile.rules.reserve(definition.rules.size());
            for (const auto& source_rule : definition.rules) {
                nlohmann::json options = nlohmann::json::object();
                for (const auto& [name, value] : source_rule.options) {
                    options[name] = value;
                }
                StoredRule rule;
                rule.key = source_rule.storage_key;
                rule.rule_id = source_rule.id.value;
                rule.enabled = source_rule.enabled;
                rule.type = source_rule.type;
                rule.options_json = options.dump();
                profile.rules.push_back(std::move(rule));
            }
            ordered.emplace_back(
                definition.storage_position.value_or(
                    std::numeric_limits<std::size_t>::max()),
                std::move(profile));
        }
        std::stable_sort(
            ordered.begin(), ordered.end(), [](const auto& left, const auto& right) {
                return left.first < right.first;
            });
        for (auto& [position, profile] : ordered) {
            (void)position;
            converted.push_back(std::move(profile));
        }
    } catch (const nlohmann::json::exception& exception) {
        error = "failed to canonicalize rule options: " + std::string(exception.what());
        return false;
    }
    profiles = std::move(converted);
    return true;
}

bool configuration_snapshot_to_config_document(
    const ConfigurationSnapshot& snapshot,
    ConfigDocument& document,
    ConfigDocumentValidationFailure& failure) {
    failure = {};
    ConfigDocument converted;
    converted.application = snapshot.application;
    for (std::size_t position = 0; position < snapshot.profiles.size(); ++position) {
        const auto& stored = snapshot.profiles[position];
        ProfileDefinition profile;
        profile.storage_key = stored.key;
        profile.storage_position = position;
        profile.enabled = stored.enabled;
        if (stored.protocol) {
            profile.protocol = ProtocolId{*stored.protocol};
        }
        profile.local.request_path = stored.local_request_path;
        profile.local.usage_path = stored.local_usage_path;
        profile.upstream.base_url = stored.upstream_base_url;
        profile.upstream.request_path = stored.upstream_request_path;
        profile.upstream.usage_path = stored.upstream_usage_path;
        profile.rules.reserve(stored.rules.size());
        for (const auto& stored_rule : stored.rules) {
            RuleDefinition rule;
            std::string error;
            if (!stored_rule_to_definition(stored_rule, rule, error)) {
                failure.profile_id = stored.profile_id;
                failure.field = "rules";
                failure.detail = "profile " + stored.profile_id + ": " + error;
                return false;
            }
            profile.rules.push_back(std::move(rule));
        }
        if (!converted.profiles.emplace(stored.profile_id, std::move(profile)).second) {
            failure.profile_id = stored.profile_id;
            failure.field = "id";
            failure.detail = "configuration snapshot contains duplicate profile id: "
                + stored.profile_id;
            return false;
        }
    }
    if (!validate_config_document(converted, failure)) {
        return false;
    }
    document = std::move(converted);
    return true;
}

bool configuration_snapshot_to_config_document(
    const ConfigurationSnapshot& snapshot,
    ConfigDocument& document,
    std::string& error) {
    ConfigDocumentValidationFailure failure;
    const bool converted = configuration_snapshot_to_config_document(
        snapshot, document, failure);
    error = std::move(failure.detail);
    return converted;
}

} // namespace ccs
