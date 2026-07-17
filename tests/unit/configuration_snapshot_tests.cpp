#include "config/config_document.hpp"
#include "config/configuration_conversion.hpp"
#include "config/configuration_snapshot.hpp"
#include "config/runtime_compiler.hpp"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

ccs::ConfigDocument fixture_document() {
    ccs::ConfigDocument document;
    document.application.runtime.max_inflight_bytes = 256ULL * 1024 * 1024;
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = "/findcg/v1/responses";
    profile.upstream.base_url = "https://example.test/v1";
    profile.upstream.request_path = "/responses";
    ccs::RuleDefinition rule;
    rule.id.value = "remove-image";
    rule.enabled = true;
    rule.type = "remove_tool";
    rule.options["tool"] = "image_gen";
    profile.rules.push_back(std::move(rule));
    document.profiles.emplace("findcg", std::move(profile));
    return document;
}

} // namespace

int main() {
    const auto legacy = fixture_document();
    std::vector<ccs::StoredProfile> profiles;
    std::string error;
    require(ccs::config_document_to_stored_profiles(legacy, profiles, error), error);
    require(profiles.size() == 1 && profiles[0].key == 0,
            "legacy import creates unassigned profile key");
    require(profiles[0].rules.size() == 1
                && profiles[0].rules[0].options_json == "{\"tool\":\"image_gen\"}",
            "legacy Rule options become canonical JSON object");

    profiles[0].key = 17;
    profiles[0].rules[0].key = 31;
    ccs::ConfigurationSnapshot configuration;
    configuration.application = legacy.application;
    configuration.profiles = profiles;
    configuration.revision.application_source = {true, "v3 bytes"};
    configuration.revision.profile_revision = 4;

    ccs::ConfigDocument round_trip;
    require(
        ccs::configuration_snapshot_to_config_document(configuration, round_trip, error),
        error);
    require(round_trip.application == legacy.application,
            "application settings round-trip through configuration snapshot");
    require(round_trip.profiles.size() == 1
                && round_trip.profiles.at("findcg").rules[0].options.at("tool") == "image_gen",
            "profiles and rules round-trip through configuration snapshot");

    ccs::RuntimeCompiler compiler(std::filesystem::temp_directory_path());
    ccs::RuntimeSnapshotPtr runtime;
    require(compiler.compile(configuration, {}, runtime, error), error);
    require(runtime->profiles.size() == 1 && runtime->routes.size() == 1,
            "RuntimeCompiler consumes combined configuration snapshot");
    require(runtime->application.runtime.max_inflight_bytes == 256ULL * 1024 * 1024,
            "runtime retains configured inflight budget");

    auto invalid = configuration;
    invalid.profiles[0].rules[0].options_json = "{ \"tool\": \"image_gen\" }";
    require(!ccs::configuration_snapshot_to_config_document(invalid, round_trip, error),
            "configuration snapshot rejects non-canonical Rule options");

    std::cout << "configuration snapshot tests passed\n";
    return 0;
}
