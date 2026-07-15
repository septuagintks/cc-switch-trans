#include "config/app_paths.hpp"
#include "config/config_document.hpp"
#include "config/config_store.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

std::filesystem::path fixture_path(const std::filesystem::path& relative) {
    return std::filesystem::path(CCS_TRANS_TEST_FIXTURE_DIR) / relative;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("failed to read test file: " + path.string());
    }
    return {
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    if (!output) {
        throw std::runtime_error("failed to write test file: " + path.string());
    }
}

std::string fixture_content() {
    return read_file(fixture_path("stage11/config-v2-roundtrip.json"));
}

ccs::ConfigDocument fixture_document() {
    ccs::ConfigDocument document;
    std::string error;
    require(ccs::parse_config_document(fixture_content(), document, error), error);
    return document;
}

bool parse_json(const nlohmann::json& source, ccs::ConfigDocument& document, std::string& error) {
    return ccs::parse_config_document(source.dump(), document, error);
}

std::filesystem::path unique_test_root(const std::string& label) {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return std::filesystem::temp_directory_path()
        / ("ccs-trans-" + label + "-" + std::to_string(nonce));
}

void test_round_trip_and_defaults() {
    auto document = fixture_document();
    require(document.application.listener.host == "127.0.0.1", "listener host parsed");
    require(document.application.listener.port == 15723, "listener port parsed");
    require(document.application.runtime.worker_threads == 32, "worker count parsed");
    require(document.profiles.size() == 1, "profile count parsed");
    const auto& findcg = document.profiles.at("findcg");
    require(findcg.enabled, "enabled profile parsed");
    require(findcg.protocol && findcg.protocol->value == "responses", "protocol parsed");
    require(findcg.rules.size() == 1, "rule count parsed");
    require(findcg.rules[0].options.at("tool") == "image_gen", "typed rule option parsed");

    std::string serialized;
    std::string error;
    require(ccs::serialize_config_document(document, serialized, error), error);
    const auto serialized_json = nlohmann::json::parse(serialized);
    require(serialized_json["schema_version"] == "ccs-trans.config/v2", "v2 schema serialized");

    ccs::ConfigDocument reparsed;
    require(ccs::parse_config_document(serialized, reparsed, error), error);
    std::string reserialized;
    require(ccs::serialize_config_document(reparsed, reserialized, error), error);
    require(reserialized == serialized, "config round-trip is canonical");

    const auto defaults = ccs::make_default_config_document();
    require(defaults.profiles.empty(), "default document has no profiles");
    require(defaults.application.runtime.worker_threads == 32, "default workers remain 32");
    require(defaults.application.logging.path == "logs/ccs-trans.log", "default log path");
    require(defaults.application.logging.max_total_size == 2ULL * 1024 * 1024 * 1024,
        "default log retention is 2 GiB");
    require(ccs::serialize_config_document(defaults, serialized, error), error);
    require(ccs::parse_config_document(serialized, reparsed, error), error);

    auto without_log_limit = nlohmann::json::parse(fixture_content());
    without_log_limit["logging"].erase("max_total_size");
    require(parse_json(without_log_limit, reparsed, error), error);
    require(reparsed.application.logging.max_total_size == 2ULL * 1024 * 1024 * 1024,
        "v2 config without max_total_size uses the new default");
}

void test_strict_json_schema() {
    const auto base = nlohmann::json::parse(fixture_content());
    ccs::ConfigDocument document;
    std::string error = "stale";

    auto invalid = base;
    invalid["unknown"] = true;
    require(!parse_json(invalid, document, error) && error.find("unknown field") != std::string::npos,
        "unknown root field rejected");

    invalid = base;
    invalid["runtime"]["threads"] = 4;
    require(!parse_json(invalid, document, error) && error.find("unknown field") != std::string::npos,
        "unknown nested field rejected");

    invalid = base;
    invalid["profiles"]["findcg"]["local"]["path_prefix"] = "/findcg";
    require(!parse_json(invalid, document, error) && error.find("unknown field") != std::string::npos,
        "unknown profile structure field rejected");

    invalid = base;
    invalid["logging"]["body"] = "true";
    require(!parse_json(invalid, document, error) && error.find("JSON boolean") != std::string::npos,
        "boolean strings rejected");

    invalid = base;
    invalid["logging"]["max_total_size"] = 1024;
    require(!parse_json(invalid, document, error)
            && error.find("too small") != std::string::npos,
        "log total size must hold a complete bounded record");

    invalid = base;
    invalid["logging"]["max_total_size"] = 1024ULL * 1024 * 1024 * 1024 + 1;
    require(!parse_json(invalid, document, error)
            && error.find("supported maximum") != std::string::npos,
        "log total size upper bound is enforced");

    invalid = base;
    invalid["listener"]["port"] = -1;
    require(!parse_json(invalid, document, error) && error.find("non-negative JSON integer") != std::string::npos,
        "negative port rejected before narrowing");

    invalid = base;
    invalid["runtime"]["worker_threads"] = 1.5;
    require(!parse_json(invalid, document, error) && error.find("JSON integer") != std::string::npos,
        "floating integer field rejected");

    invalid = base;
    invalid["timeouts"]["resolve_ms"] = 2147483648ULL;
    require(!parse_json(invalid, document, error) && error.find("supported maximum") != std::string::npos,
        "integer overflow rejected before int conversion");

    invalid = base;
    invalid.erase("profiles");
    require(!parse_json(invalid, document, error) && error.find("missing required field") != std::string::npos,
        "missing root field rejected");

    const auto original = fixture_document();
    document = original;
    require(!ccs::parse_config_document("{bad json", document, error), "malformed JSON rejected");
    require(document.profiles.count("findcg") == 1, "failed parse leaves output document unchanged");

    auto duplicate = fixture_content();
    const auto marker = duplicate.find("\"schema_version\"");
    require(marker != std::string::npos, "fixture schema marker");
    duplicate.insert(marker, "\"schema_version\": \"ccs-trans.config/v2\",\n  ");
    require(!ccs::parse_config_document(duplicate, document, error)
            && error.find("duplicate JSON object key") != std::string::npos,
        "duplicate JSON keys rejected");
}

void test_drafts_and_enabled_validation() {
    ccs::ConfigDocument document = ccs::make_default_config_document();
    document.profiles.emplace("draft", ccs::ProfileDefinition{});
    std::string serialized;
    std::string error;
    require(ccs::serialize_config_document(document, serialized, error), error);
    const auto draft_json = nlohmann::json::parse(serialized);
    require(draft_json["profiles"]["draft"]["enabled"] == false, "draft defaults disabled");
    require(draft_json["profiles"]["draft"]["rules"].empty(), "draft defaults to no rules");

    auto candidate = document;
    candidate.profiles.at("draft").enabled = true;
    require(!ccs::validate_config_document(candidate, error)
            && error.find("is enabled but missing") != std::string::npos,
        "incomplete enabled profile rejected");

    auto& draft = document.profiles.at("draft");
    draft.protocol = ccs::ProtocolId{"responses"};
    draft.local.request_path = "/draft/v1/responses";
    draft.upstream.base_url = "https://example.com";
    draft.upstream.request_path = "/v1/responses";
    draft.local.usage_path = "/draft/v1/usage";
    require(ccs::validate_config_document(document, error), error);
    draft.enabled = true;
    require(!ccs::validate_config_document(document, error)
            && error.find("both local.usage_path") != std::string::npos,
        "enabled profile requires paired Usage paths");
    draft.upstream.usage_path = "/v1/usage";
    require(ccs::validate_config_document(document, error), error);

    ccs::RuleDefinition rule;
    rule.id.value = "remove-image-gen";
    rule.type = "remove_tool";
    rule.options["tool"] = "image_gen";
    draft.rules.push_back(rule);
    require(ccs::validate_config_document(document, error), error);
    draft.rules.push_back(rule);
    require(!ccs::validate_config_document(document, error)
            && error.find("duplicate rule id") != std::string::npos,
        "duplicate rule ids rejected");
    draft.rules.pop_back();
    draft.rules[0].options["type"] = "overwrite";
    require(!ccs::validate_config_document(document, error)
            && error.find("invalid option name") != std::string::npos,
        "rule options cannot overwrite structural fields");
}

void test_identifiers_paths_and_urls() {
    require(ccs::is_valid_profile_id("findcg.desktop-1"), "profile id syntax");
    require(!ccs::is_valid_profile_id("../findcg"), "escaping profile id rejected");
    require(ccs::is_valid_protocol_id("chat_completions"), "protocol id syntax");
    require(!ccs::is_valid_protocol_id("Chat"), "protocol id is lowercase");
    require(ccs::is_valid_rule_id("remove-image-gen"), "rule id syntax");
    require(ccs::is_valid_rule_type("remove_tool"), "rule type syntax");
    require(!ccs::is_valid_rule_type("remove-tool"), "rule type uses snake case");

    auto document = fixture_document();
    auto& profile = document.profiles.at("findcg");
    std::string error;
    const auto valid_path = *profile.local.request_path;
    const std::vector<std::pair<std::string, std::string>> invalid_paths = {
        {"/v1//responses", "duplicate separators"},
        {"/v1/../responses", "dot segments"},
        {"/v1/%2e%2e/responses", "encode dot"},
        {"/v1/responses?x=1", "query"},
        {"/_ccs-trans/health", "reserved"},
    };
    for (const auto& [path, expected] : invalid_paths) {
        profile.local.request_path = path;
        require(!ccs::validate_config_document(document, error)
                && error.find(expected) != std::string::npos,
            "invalid local path rejected: " + path);
    }
    profile.local.request_path = valid_path;

    const auto valid_url = *profile.upstream.base_url;
    for (const auto& url : {
             std::string("ftp://example.com"),
             std::string("https://user@example.com"),
             std::string("https://example.com/path?query=1"),
             std::string("https://bad host.example.com")}) {
        profile.upstream.base_url = url;
        require(!ccs::validate_config_document(document, error), "invalid upstream URL rejected: " + url);
    }
    profile.upstream.base_url = "https://openrouter.ai/api";
    require(ccs::validate_config_document(document, error), error);
    profile.upstream.base_url = valid_url;

    document.application.logging.path = "../outside.log";
    require(!ccs::validate_config_document(document, error)
            && error.find("application root") != std::string::npos,
        "relative log path cannot escape application root");
#ifdef _WIN32
    document.application.logging.path = "\\outside.log";
    require(!ccs::validate_config_document(document, error)
            && error.find("application root") != std::string::npos,
        "root-relative Windows log path cannot escape application root");
#endif
    document.application.logging.path = "logs/\xE6\xB5\x8B\xE8\xAF\x95.log";
    require(ccs::validate_config_document(document, error), error);
}

void test_limits() {
    auto document = fixture_document();
    std::string error;
    document.application.runtime.worker_threads = 1025;
    require(!ccs::validate_config_document(document, error), "worker upper bound enforced");
    document = fixture_document();
    document.application.runtime.max_connections = 31;
    require(!ccs::validate_config_document(document, error)
            && error.find("worker_threads") != std::string::npos,
        "max connections must cover workers");
    document = fixture_document();
    document.application.runtime.max_request_body_size = 1024ULL * 1024 * 1024 + 1;
    require(!ccs::validate_config_document(document, error), "body limit upper bound enforced");
    document = fixture_document();
    document.application.runtime.metrics_interval_ms = 0xffffffffU;
    require(!ccs::validate_config_document(document, error), "metrics integer upper bound enforced");

    document = fixture_document();
    document.profiles.at("findcg").rules[0].options["ratio"]
        = std::numeric_limits<double>::quiet_NaN();
    require(!ccs::validate_config_document(document, error)
            && error.find("JSON text") != std::string::npos,
        "non-finite rule option rejected");

    document = ccs::make_default_config_document();
    for (std::size_t i = 0; i <= ccs::kMaxConfigProfiles; ++i) {
        document.profiles.emplace("profile-" + std::to_string(i), ccs::ProfileDefinition{});
    }
    require(!ccs::validate_config_document(document, error)
            && error.find("maximum of 128 profiles") != std::string::npos,
        "profile count limit enforced");

    document = ccs::make_default_config_document();
    auto& profile = document.profiles["rules"];
    for (std::size_t i = 0; i <= ccs::kMaxRulesPerProfile; ++i) {
        ccs::RuleDefinition rule;
        rule.id.value = "rule-" + std::to_string(i);
        rule.type = "set_field";
        profile.rules.push_back(std::move(rule));
    }
    require(!ccs::validate_config_document(document, error)
            && error.find("maximum rule count") != std::string::npos,
        "rule count limit enforced");

    auto oversized = fixture_content();
    oversized.append(ccs::kMaxConfigDocumentBytes, ' ');
    require(!ccs::parse_config_document(oversized, document, error)
            && error.find("4 MiB") != std::string::npos,
        "document byte limit enforced before parsing");
}

void test_config_store_atomicity_and_revision() {
    const auto root = unique_test_root("config-v2-store");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto paths = ccs::make_app_paths(root);
    std::string error;

    ccs::ConfigStore store(paths);
    require(store.load(error), error);
    require(store.loaded() && store.document().profiles.empty(), "missing config loads defaults");
    auto document = fixture_document();
    require(store.save(document, error), error);
    require(std::filesystem::exists(paths.config_file), "v2 config atomically created");
    require(std::filesystem::exists(paths.state_directory / "config.lock"), "cross-process lock file created");
    const auto original_bytes = read_file(paths.config_file);

    for (const auto& entry : std::filesystem::directory_iterator(paths.root)) {
        require(entry.path().filename().string().find("config.json.tmp-") == std::string::npos,
            "atomic save leaves no temporary file");
    }

    ccs::ConfigStore first(paths);
    ccs::ConfigStore stale(paths);
    require(first.load(error), error);
    require(stale.load(error), error);
    auto first_edit = first.document();
    first_edit.application.logging.level = "debug";
    require(first.save(first_edit, error), error);
    const auto first_bytes = read_file(paths.config_file);
    auto stale_edit = stale.document();
    stale_edit.application.logging.body = false;
    require(!stale.save(stale_edit, error)
            && error.find("changed since it was loaded") != std::string::npos,
        "stale writer cannot overwrite a newer document");
    require(stale.last_failure() == ccs::ConfigRepositoryFailure::Stale,
        "stale config save exposes a structured repository failure");
    require(read_file(paths.config_file) == first_bytes, "stale save leaves newer bytes unchanged");

    auto invalid = first.document();
    invalid.application.runtime.worker_threads = 0;
    require(!first.save(invalid, error), "invalid candidate cannot be saved");
    require(first.last_failure() == ccs::ConfigRepositoryFailure::InvalidDocument,
        "invalid config save exposes a structured repository failure");
    require(read_file(paths.config_file) == first_bytes, "invalid save leaves target unchanged");

#ifdef _WIN32
    HANDLE held_lock = CreateFileW(
        (paths.state_directory / "config.lock").c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    require(held_lock != INVALID_HANDLE_VALUE, "test acquired external config lock");
    auto locked_edit = first.document();
    locked_edit.application.logging.level = "warn";
    require(!first.save(locked_edit, error)
            && error.find("another process") != std::string::npos,
        "active cross-process writer lock is respected");
    require(first.last_failure() == ccs::ConfigRepositoryFailure::Busy,
        "config lock contention exposes a structured repository failure");
    CloseHandle(held_lock);
    require(read_file(paths.config_file) == first_bytes, "lock contention leaves target unchanged");
#endif

    ccs::ConfigStore reloaded(paths);
    require(reloaded.load(error), error);
    require(reloaded.document().application.logging.level == "debug", "saved document reloaded");
    require(original_bytes != first_bytes, "successful edit replaced config bytes");
    std::filesystem::remove_all(root, ec);
}

void test_unsupported_and_oversized_files_are_preserved() {
    const auto root = unique_test_root("config-v2-unsupported");
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto paths = ccs::make_app_paths(root);
    std::filesystem::create_directories(root);
    const std::string unsupported = R"({"schema_version":"unsupported","profiles":{}})";
    write_file(paths.config_file, unsupported);

    ccs::ConfigStore store(paths);
    std::string error;
    require(!store.load(error) && error.find("expected ccs-trans.config/v2") != std::string::npos,
        "unsupported schema rejected explicitly");
    require(!store.loaded(), "failed unsupported load does not publish a document");
    require(!store.save(ccs::make_default_config_document(), error)
            && error.find("loaded successfully") != std::string::npos,
        "failed load cannot overwrite unsupported config");
    require(read_file(paths.config_file) == unsupported,
        "unsupported config bytes remain unchanged");

    std::string oversized(ccs::kMaxConfigDocumentBytes + 1, 'x');
    write_file(paths.config_file, oversized);
    ccs::ConfigStore oversized_store(paths);
    require(!oversized_store.load(error) && error.find("4 MiB") != std::string::npos,
        "oversized file rejected before full read");
    require(read_file(paths.config_file) == oversized, "oversized file remains unchanged");
    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"round trip and defaults", test_round_trip_and_defaults},
        {"strict JSON schema", test_strict_json_schema},
        {"draft and enabled validation", test_drafts_and_enabled_validation},
        {"identifiers paths and URLs", test_identifiers_paths_and_urls},
        {"limits", test_limits},
        {"config store atomicity and revision", test_config_store_atomicity_and_revision},
        {"unsupported and oversized preservation", test_unsupported_and_oversized_files_are_preserved},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "ok: " << name << "\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "config document tests failed: " << ex.what() << "\n";
        return 1;
    }
}
