#include "config/composite_config_repository.hpp"
#include "config/configuration_editor.hpp"
#include "config/rules_text.hpp"
#include "../support/canonical_temp.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>
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

void test_forward_compatibility_boundary() {
    const std::string future_rule = R"({
  "schema_version": "ccs-trans.rules/v1",
  "rules": [
    {
      "id": "future",
      "enabled": false,
      "type": "future_rule",
      "options": {
        "nested": {"array": [1, true, null, {"text": "\u4fdd\u7559"}]},
        "number": 1.25
      }
    }
  ]
})";
    ccs::RulesTextError error;
    std::vector<ccs::StoredRule> parsed;
    require(ccs::parse_rules_text(future_rule, {}, parsed, error), error.message);
    const auto expected_options = nlohmann::json::parse(parsed.front().options_json);
    std::string formatted;
    require(ccs::format_rules_text(parsed, formatted, error), error.message);
    std::vector<ccs::StoredRule> reparsed;
    require(ccs::parse_rules_text(formatted, parsed, reparsed, error), error.message);
    require(reparsed.size() == 1
            && reparsed.front().type == "future_rule"
            && nlohmann::json::parse(reparsed.front().options_json) == expected_options,
        "unknown Rule types preserve arbitrary options through canonical round-trip");

    const std::string unknown_root = R"({
  "schema_version": "ccs-trans.rules/v1",
  "rules": [],
  "future": true
})";
    require(!ccs::parse_rules_text(unknown_root, {}, parsed, error)
            && error.message.find("unknown field: future") != std::string::npos,
        "unknown Rules root fields remain rejected");

    const std::string unknown_wrapper = R"({
  "schema_version": "ccs-trans.rules/v1",
  "rules": [
    {
      "id": "future",
      "enabled": false,
      "type": "future_rule",
      "options": {},
      "label": "not part of the wrapper"
    }
  ]
})";
    require(!ccs::parse_rules_text(unknown_wrapper, {}, parsed, error)
            && error.message.find("unknown field: label") != std::string::npos,
        "unknown Rule wrapper fields remain rejected");

    const std::string unknown_known_option = R"({
  "schema_version": "ccs-trans.rules/v1",
  "rules": [
    {
      "id": "remove",
      "enabled": true,
      "type": "remove_tool",
      "options": {"tool": "image_gen", "future": true}
    }
  ]
})";
    require(!ccs::parse_rules_text(unknown_known_option, {}, parsed, error)
            && error.rule_id == "remove"
            && error.rule_type == "remove_tool"
            && error.option == "future",
        "known Rule types reject options absent from their descriptor");
}

void test_clipboard_newline_normalization() {
    const std::string source =
        "{\r\n"
        "\t\"schema_version\": \"ccs-trans.rules/v1\",\xE2\x80\xA8"
        "\t\"rules\": [\xE2\x80\xA9"
        "{\"id\":\"first\",\"enabled\":false,\"type\":\"future_rule\","
        "\"options\":{\"note\":\"escaped\\nline \xE2\x80\xA8 kept\"}},\r"
        "{\"id\":\"second\",\"enabled\":false,\"type\":\"future_rule\","
        "\"options\":{\"tab\":\"a\\tb\"}}\n"
        "]\r\n}\r\n";
    const auto normalized = ccs::normalize_rules_text_newlines(source);
    require(normalized.find('\r') == std::string::npos,
        "CRLF and CR clipboard boundaries normalize to LF");
    require(normalized.find("escaped\\nline") != std::string::npos,
        "escaped JSON newline remains two source characters");
    require(normalized.find("line \xE2\x80\xA8 kept") != std::string::npos,
        "Unicode line separator inside a JSON string remains data");

    std::vector<ccs::StoredRule> parsed;
    ccs::RulesTextError error;
    require(ccs::parse_rules_text(source, {}, parsed, error), error.message);
    require(parsed.size() == 2
            && parsed[0].rule_id == "first"
            && parsed[1].rule_id == "second",
        "mixed clipboard newlines preserve Rule order");
    const auto first_options = nlohmann::json::parse(parsed[0].options_json);
    require(first_options.at("note").get<std::string>()
            == "escaped\nline \xE2\x80\xA8 kept",
        "normalization preserves escaped newline and Unicode string semantics");

    std::string formatted;
    require(ccs::format_rules_text(parsed, formatted, error), error.message);
    std::vector<ccs::StoredRule> reparsed;
    require(ccs::parse_rules_text(formatted, parsed, reparsed, error), error.message);
    require(reparsed == parsed, "clipboard fixture survives canonical round-trip");

    std::string large_note(256 * 1024, 'x');
    nlohmann::json large = {
        {"schema_version", "ccs-trans.rules/v1"},
        {"rules", nlohmann::json::array({{
            {"id", "large"},
            {"enabled", false},
            {"type", "future_rule"},
            {"options", {{"note", large_note}}},
        }})},
    };
    require(ccs::parse_rules_text(large.dump(), {}, parsed, error)
            && nlohmann::json::parse(parsed.front().options_json)
                    .at("note").get<std::string>() == large_note,
        "large clipboard-style Rule text is not truncated");
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
    test_forward_compatibility_boundary();
    test_clipboard_newline_normalization();
    test_enabled_unknown_rule_is_rejected_at_commit();
    std::cout << "rules text tests passed\n";
    return 0;
}
