#include "config/composite_config_repository.hpp"
#include "config/configuration_editor.hpp"
#include "config/field_descriptor.hpp"
#include "../support/canonical_temp.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <set>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class Fixture final {
public:
    Fixture() {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = ccs::test::canonical_temp_directory()
            / ("ccs-trans-configuration-editor-" + std::to_string(nonce));
    }

    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    std::filesystem::path root;
};

void test_descriptor_contract() {
    std::set<std::string_view> keys;
    for (const auto& descriptor : ccs::application_field_descriptors()) {
        require(descriptor.scope == ccs::ConfigurationFieldScope::Application,
            "application descriptor scope");
        require(!descriptor.key.empty() && !descriptor.display_name_key.empty(),
            "application descriptor names are present");
        require(keys.emplace(descriptor.key).second, "application descriptor keys are unique");
    }
    require(keys.count("runtime.max-inflight-bytes") == 1,
        "inflight budget has a shared descriptor");
    const auto* inflight = ccs::find_configuration_field_descriptor(
        ccs::ConfigurationFieldScope::Application, "runtime.max-inflight-bytes");
    require(inflight != nullptr, "find inflight descriptor");
    require(inflight->apply_impact == ccs::RuntimeApplyImpact::ServiceRestart,
        "inflight budget declares restart impact");

    ccs::ConfigurationFieldValue value;
    std::string error;
    require(ccs::parse_configuration_field_value(*inflight, "805306368", value, error),
        error);
    require(std::get<std::uint64_t>(value) == 805306368ULL,
        "numeric descriptor returns typed value");
    require(!ccs::parse_configuration_field_value(*inflight, "1048575", value, error),
        "numeric descriptor enforces lower bound");

    const auto* protocol = ccs::find_configuration_field_descriptor(
        ccs::ConfigurationFieldScope::Profile, "protocol");
    require(protocol != nullptr && !protocol->required,
        "profile protocol descriptor is optional for a draft");
    require(!ccs::parse_configuration_field_value(*protocol, "unknown", value, error),
        "enumeration descriptor rejects unknown values");
    require(ccs::parse_configuration_field_value(*protocol, "responses", value, error),
        error);
}

ccs::SetConfigurationFieldCommand profile_field(
    ccs::ProfileKey key,
    std::string field,
    ccs::ConfigurationFieldValue value) {
    return {key, std::move(field), std::move(value)};
}

void test_editor_stable_keys_and_stale_revision() {
    Fixture fixture;
    const auto paths = ccs::make_app_paths(fixture.root);
    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(repository.load(error), error);

    ccs::ConfigurationEditor create(repository);
    require(create.begin(error), error);
    ccs::ProfileKey first_draft_key = 0;
    ccs::ProfileKey second_draft_key = 0;
    require(create.create_profile("draft", first_draft_key, error), error);
    require(create.create_profile("discard-before-commit", second_draft_key, error), error);
    require(first_draft_key < 0 && second_draft_key < 0
            && first_draft_key != second_draft_key,
        "multiple unsaved profiles receive distinct draft keys");
    require(create.apply(profile_field(
        first_draft_key, "id", std::string("renamed-draft")), error), error);
    require(create.remove_profile(second_draft_key, error), error);
    ccs::ConfigurationSnapshot committed;
    require(create.commit(committed, error), error);
    require(committed.profiles.size() == 1 && committed.profiles.front().key > 0,
        "profile create receives a stable key");
    require(committed.profiles.front().profile_id == "renamed-draft",
        "draft key remains stable through rename");
    const auto profile_key = committed.profiles.front().key;

    ccs::ConfigurationEditor editor(repository);
    require(editor.begin(error), error);
    require(editor.apply(
        {std::nullopt, "runtime.max-inflight-bytes", std::uint64_t{805306368}}, error), error);
    require(editor.apply(profile_field(profile_key, "id", std::string("primary")), error), error);
    require(editor.apply(profile_field(profile_key, "protocol", std::string("responses")), error), error);
    require(editor.apply(profile_field(
        profile_key, "local.request-path", std::string("/primary/v1/responses")), error), error);
    require(editor.apply(profile_field(
        profile_key, "local.usage-path", std::string("/primary/v1/usage")), error), error);
    require(editor.apply(profile_field(
        profile_key, "upstream.base-url", std::string("https://example.test/v1")), error), error);
    require(editor.apply(profile_field(
        profile_key, "upstream.request-path", std::string("/responses")), error), error);
    require(editor.apply(profile_field(
        profile_key, "upstream.usage-path", std::string("/usage")), error), error);
    require(editor.add_rule(profile_key, "remove-image", "remove_tool", error), error);
    require(editor.draft().profiles.front().rules.front().key < 0,
        "new rule receives a draft-stable key before commit");
    require(editor.set_rule_option(
        profile_key, "remove-image", "tool", "\"image_gen\"", error), error);
    require(editor.set_rule_enabled(profile_key, "remove-image", true, error), error);
    require(editor.commit(committed, error), error);
    require(committed.profiles.front().key == profile_key,
        "profile rename preserves stable key");
    require(committed.profiles.front().rules.front().key > 0,
        "new rule receives a stable key");
    const auto rule_key = committed.profiles.front().rules.front().key;

    ccs::CompositeConfigRepository first_repository(paths);
    ccs::CompositeConfigRepository stale_repository(paths);
    require(first_repository.load(error), error);
    require(stale_repository.load(error), error);
    ccs::ConfigurationEditor first(first_repository);
    ccs::ConfigurationEditor stale(stale_repository);
    require(first.begin(error), error);
    require(stale.begin(error), error);
    require(first.apply(
        {std::nullopt, "logging.level", std::string("debug")}, error), error);
    require(first.commit(committed, error), error);
    require(stale.apply(
        {std::nullopt, "logging.level", std::string("trace")}, error), error);
    require(!stale.commit(committed, error), "stale editor commit is rejected");
    require(stale_repository.last_failure() == ccs::ConfigRepositoryFailure::Stale,
        "stale editor preserves repository failure");

    ccs::CompositeConfigRepository reloaded(paths);
    require(reloaded.load(error), error);
    require(reloaded.snapshot().profiles.front().key == profile_key,
        "profile key round-trips");
    require(reloaded.snapshot().profiles.front().rules.front().key == rule_key,
        "rule key round-trips");
}

} // namespace

int main() {
    test_descriptor_contract();
    test_editor_stable_keys_and_stale_revision();
    std::cout << "configuration editor tests passed\n";
    return 0;
}
