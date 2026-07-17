#include "config/composite_config_repository.hpp"
#include "config/configuration_editor.hpp"
#include "config/rules_text.hpp"
#include "../support/canonical_temp.hpp"

#include <chrono>
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

void test_canonical_round_trip_and_stable_keys() {
    const std::vector<ccs::StoredRule> existing = {
        {41, "remove-image", false, "remove_tool", "{}"},
        {42, "old-rule", false, "unknown_rule", "{\"note\":\"keep\"}"},
    };
    const std::string source =
        "{\n"
        "  \"schema_version\": \"ccs-trans.rules/v1\",\n"
        "  \"rules\": [\n"
        "    {\n"
        "      \"id\": \"remove-image\",\n"
        "      \"enabled\": true,\n"
        "      \"type\": \"remove_tool\",\n"
        "      \"options\": {\"tool\": \"image_gen\"}\n"
        "    },\n"
        "    {\n"
        "      \"id\": \"new-disabled\",\n"
        "      \"enabled\": false,\n"
        "      \"type\": \"future_rule\",\n"
        "      \"options\": {\"note\": \"\xE4\xBF\x9D\xE7\x95\x99\"}\n"
        "    }\n"
        "  ]\n"
        "}\n";

    std::vector<ccs::StoredRule> parsed;
    ccs::RulesTextError error;
    require(ccs::parse_rules_text(source, existing, parsed, error), error.message);
    require(parsed.size() == 2, "two rules parsed");
    require(parsed[0].key == 41, "same rule id preserves stable key");
    require(parsed[1].key == 0, "new rule id receives a new key");
    require(parsed[1].type == "future_rule" && !parsed[1].enabled,
        "disabled unknown rule round-trips");

    std::string formatted;
    require(ccs::format_rules_text(parsed, formatted, error), error.message);
    require(formatted.find("\r") == std::string::npos, "canonical text uses LF");
    require(formatted.find("\"id\": \"remove-image\"")
            < formatted.find("\"enabled\": true"),
        "canonical rule field order starts with id");
    require(formatted.find("\xE4\xBF\x9D\xE7\x95\x99") != std::string::npos,
        "canonical text retains UTF-8 without ASCII escaping");

    std::vector<ccs::StoredRule> reparsed;
    require(ccs::parse_rules_text(formatted, parsed, reparsed, error), error.message);
    require(reparsed == parsed, "canonical rules text round-trips exactly");
}

void test_diagnostics_and_limits() {
    ccs::RulesTextError error;
    std::vector<ccs::StoredRule> parsed;
    const std::string syntax_error =
        "{\n  \"schema_version\": \"ccs-trans.rules/v1\",\n  \"rules\": [}\n";
    require(!ccs::parse_rules_text(syntax_error, {}, parsed, error),
        "syntax error rejected");
    require(error.line == 3 && error.column > 1, "syntax error has line and column");

    const std::string duplicate_id =
        "{\"schema_version\":\"ccs-trans.rules/v1\",\"rules\":["
        "{\"id\":\"same\",\"enabled\":false,\"type\":\"x\",\"options\":{}},"
        "{\"id\":\"same\",\"enabled\":false,\"type\":\"y\",\"options\":{}}]}";
    require(!ccs::parse_rules_text(duplicate_id, {}, parsed, error),
        "duplicate rule id rejected");
    require(error.rule_id == "same", "duplicate diagnostic identifies rule id");

    const std::string wrong_option =
        "{\"schema_version\":\"ccs-trans.rules/v1\",\"rules\":["
        "{\"id\":\"remove\",\"enabled\":true,\"type\":\"remove_tool\","
        "\"options\":{\"tool\":7}}]}";
    require(!ccs::parse_rules_text(wrong_option, {}, parsed, error),
        "descriptor option type rejected");
    require(error.rule_id == "remove" && error.rule_type == "remove_tool"
            && error.option == "tool",
        "descriptor diagnostic carries rule context");

    std::string oversized(ccs::kMaxStoredProfileRulesTextBytes + 1, ' ');
    require(!ccs::parse_rules_text(oversized, {}, parsed, error),
        "rules text size limit enforced");
}

void test_enabled_unknown_rule_is_rejected_at_commit() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = ccs::test::canonical_temp_directory()
        / ("ccs-trans-rules-text-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(repository.load(error), error);
    auto desired = repository.snapshot();
    ccs::StoredProfile profile;
    profile.profile_id = "test";
    profile.enabled = true;
    profile.protocol = "responses";
    profile.local_request_path = "/v1/responses";
    profile.upstream_base_url = "https://example.test/v1";
    profile.upstream_request_path = "/responses";
    desired.profiles.push_back(std::move(profile));
    ccs::ConfigurationSnapshot committed;
    require(repository.save_snapshot(desired, committed, error), error);
    const auto profile_key = committed.profiles.front().key;

    const std::string unknown =
        "{\"schema_version\":\"ccs-trans.rules/v1\",\"rules\":["
        "{\"id\":\"future\",\"enabled\":true,\"type\":\"future_rule\","
        "\"options\":{}}]}";
    std::vector<ccs::StoredRule> parsed;
    ccs::RulesTextError parse_error;
    require(ccs::parse_rules_text(unknown, {}, parsed, parse_error), parse_error.message);
    require(parsed.front().enabled, "enabled unknown rule remains a valid text draft");

    ccs::ConfigurationEditor editor(repository);
    require(editor.begin(error), error);
    require(editor.replace_rules_text(profile_key, unknown, parse_error, error), error);
    require(!editor.commit(committed, error), "runtime compiler rejects enabled unknown rule");
    require(error.find("unknown type") != std::string::npos,
        "enabled unknown rule reports runtime validation error");

    std::error_code ec;
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    test_canonical_round_trip_and_stable_keys();
    test_diagnostics_and_limits();
    test_enabled_unknown_rule_is_rejected_at_commit();
    std::cout << "rules text tests passed\n";
    return 0;
}
