#include "config/app_paths.hpp"
#include "config/config_cli.hpp"
#include "config/composite_config_repository.hpp"
#include "config/config_store.hpp"
#include "../support/canonical_temp.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ccs::ConfigCliParseResult parse(const std::vector<std::string>& arguments) {
    std::vector<std::string> storage;
    storage.reserve(arguments.size() + 1);
    storage.push_back("ccs-trans");
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (auto& value : storage) {
        argv.push_back(value.data());
    }
    return ccs::parse_config_cli(static_cast<int>(argv.size()), argv.data());
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

std::filesystem::path unique_test_root() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    return ccs::test::canonical_temp_directory()
        / ("ccs-trans-config-cli-" + std::to_string(nonce));
}

std::string execute(
    ccs::ConfigStore& store,
    const std::vector<std::string>& arguments) {
    const auto parsed = parse(arguments);
    require(parsed.ok, parsed.error);
    std::string output;
    std::string error;
    require(ccs::execute_config_cli(parsed.command, store, output, error), error);
    return output;
}

bool execute_fails(
    ccs::ConfigStore& store,
    const std::vector<std::string>& arguments,
    std::string& error) {
    const auto parsed = parse(arguments);
    if (!parsed.ok) {
        error = parsed.error;
        return true;
    }
    std::string output;
    return !ccs::execute_config_cli(parsed.command, store, output, error);
}

std::string execute_composite(
    ccs::CompositeConfigRepository& repository,
    const std::vector<std::string>& arguments) {
    const auto parsed = parse(arguments);
    require(parsed.ok, parsed.error);
    std::string output;
    std::string error;
    require(
        ccs::execute_config_cli(parsed.command, repository, output, error),
        error);
    return output;
}

void set_complete_profile(
    ccs::ConfigStore& store,
    const std::string& id,
    const std::string& prefix) {
    execute(store, {"profile", "set", id, "protocol", "responses"});
    execute(store, {"profile", "set", id, "local.request-path", prefix + "/v1/responses"});
    execute(store, {"profile", "set", id, "local.usage-path", prefix + "/v1/usage"});
    execute(store, {"profile", "set", id, "upstream.base-url", "https://example.com/api"});
    execute(store, {"profile", "set", id, "upstream.request-path", "/v1/responses"});
    execute(store, {"profile", "set", id, "upstream.usage-path", "/v1/usage"});
}

void test_parser_contract() {
    const std::vector<std::pair<std::vector<std::string>, ccs::ConfigCliCommandKind>> valid = {
        {{"config", "show"}, ccs::ConfigCliCommandKind::ConfigShow},
        {{"config", "set", "listener.port", "16000"}, ccs::ConfigCliCommandKind::ConfigSet},
        {{"config", "unset", "listener.port"}, ccs::ConfigCliCommandKind::ConfigUnset},
        {{"profile", "list"}, ccs::ConfigCliCommandKind::ProfileList},
        {{"profile", "show", "findcg"}, ccs::ConfigCliCommandKind::ProfileShow},
        {{"profile", "create", "findcg"}, ccs::ConfigCliCommandKind::ProfileCreate},
        {{"profile", "remove", "findcg"}, ccs::ConfigCliCommandKind::ProfileRemove},
        {{"profile", "enable", "findcg"}, ccs::ConfigCliCommandKind::ProfileEnable},
        {{"profile", "disable", "findcg"}, ccs::ConfigCliCommandKind::ProfileDisable},
        {{"profile", "set", "findcg", "protocol", "responses"}, ccs::ConfigCliCommandKind::ProfileSet},
        {{"profile", "unset", "findcg", "protocol"}, ccs::ConfigCliCommandKind::ProfileUnset},
        {{"profile", "rename", "findcg", "primary"}, ccs::ConfigCliCommandKind::ProfileRename},
        {{"profile", "move", "findcg", "1"}, ccs::ConfigCliCommandKind::ProfileMove},
        {{"rule", "list", "findcg"}, ccs::ConfigCliCommandKind::RuleList},
        {{"rule", "show", "findcg", "remove-image"}, ccs::ConfigCliCommandKind::RuleShow},
        {{"rule", "add", "findcg", "remove-image", "remove_tool"}, ccs::ConfigCliCommandKind::RuleAdd},
        {{"rule", "remove", "findcg", "remove-image"}, ccs::ConfigCliCommandKind::RuleRemove},
        {{"rule", "enable", "findcg", "remove-image"}, ccs::ConfigCliCommandKind::RuleEnable},
        {{"rule", "disable", "findcg", "remove-image"}, ccs::ConfigCliCommandKind::RuleDisable},
        {{"rule", "set", "findcg", "remove-image", "tool", "image_gen"}, ccs::ConfigCliCommandKind::RuleSet},
        {{"rule", "unset", "findcg", "remove-image", "tool"}, ccs::ConfigCliCommandKind::RuleUnset},
        {{"rule", "move", "findcg", "remove-image", "1"}, ccs::ConfigCliCommandKind::RuleMove},
        {{"storage", "status"}, ccs::ConfigCliCommandKind::StorageStatus},
        {{"storage", "migrate"}, ccs::ConfigCliCommandKind::StorageMigrate},
        {{"storage", "migrate", "--replace"}, ccs::ConfigCliCommandKind::StorageMigrate},
        {{"storage", "migrate", "--replace", "--confirm", "REPLACE"},
            ccs::ConfigCliCommandKind::StorageMigrate},
        {{"storage", "verify"}, ccs::ConfigCliCommandKind::StorageVerify},
        {{"run", "--profile", "findcg", "--log-level", "debug", "--log-path", "logs/debug.log"}, ccs::ConfigCliCommandKind::Run},
    };
    for (const auto& [arguments, kind] : valid) {
        const auto result = parse(arguments);
        require(result.ok && result.command.kind == kind, "valid command parsed: " + arguments[0]);
    }

    const auto run = parse({"run", "--profile", "findcg", "--log-level", "debug", "--log-path", "logs/debug.log"});
    require(run.command.run_profile == "findcg", "run profile parsed");
    require(run.command.run_log_level == "debug", "run log level parsed");
    require(run.command.run_log_path == "logs/debug.log", "run log path parsed");

    const std::vector<std::vector<std::string>> invalid = {
        {},
        {"config", "show", "extra"},
        {"config", "set", "unknown.key", "x"},
        {"profile", "select", "findcg"},
        {"profile", "set", "findcg", "enabled", "true"},
        {"profile", "create", "../escape"},
        {"rule", "add", "findcg", "rule", "Bad-Type"},
        {"rule", "set", "findcg", "rule", "enabled", "true"},
        {"rule", "move", "findcg", "rule", "0"},
        {"storage"},
        {"storage", "repair"},
        {"storage", "status", "extra"},
        {"storage", "migrate", "--confirm", "REPLACE"},
        {"storage", "migrate", "--replace", "--confirm"},
        {"run", "--unknown", "value"},
        {"run", "--profile", "a", "--profile", "b"},
        {"run", "--log-level", "verbose"},
        {"-h"},
    };
    for (const auto& arguments : invalid) {
        require(!parse(arguments).ok, "invalid command rejected");
    }

    require(ccs::is_config_cli_management_command("config"), "config command classified");
    require(ccs::is_config_cli_management_command("profile"), "profile command classified");
    require(ccs::is_config_cli_management_command("rule"), "rule command classified");
    require(ccs::is_config_cli_management_command("storage"), "storage command classified");
    require(!ccs::is_config_cli_management_command("run"), "run remains a host command");
}

void test_storage_replacement_confirmation() {
    auto parsed = parse({"storage", "migrate", "--replace"});
    require(parsed.ok && parsed.command.storage_replace, "replace mode parsed");
    std::string error;
    std::istringstream noninteractive("REPLACE\n");
    std::ostringstream prompt;
    require(!ccs::confirm_storage_replacement(
                parsed.command, false, noninteractive, prompt, error)
            && error.find("--confirm REPLACE") != std::string::npos
            && prompt.str().empty(),
        "non-interactive replacement never reads stdin without an explicit token");

    std::istringstream interactive("REPLACE\n");
    require(ccs::confirm_storage_replacement(
                parsed.command, true, interactive, prompt, error)
            && prompt.str() == "Type REPLACE to continue: ",
        "interactive replacement requires the exact prompt response");
    std::istringstream wrong_case("replace\n");
    std::ostringstream ignored_prompt;
    require(!ccs::confirm_storage_replacement(
                parsed.command, true, wrong_case, ignored_prompt, error),
        "interactive confirmation is case-sensitive");

    parsed = parse({"storage", "migrate", "--replace", "--confirm", "REPLACE"});
    std::istringstream empty;
    require(ccs::confirm_storage_replacement(
                parsed.command, false, empty, ignored_prompt, error),
        "non-interactive exact token is accepted");
    parsed = parse({"storage", "migrate", "--replace", "--confirm", "replace"});
    require(parsed.ok
            && !ccs::confirm_storage_replacement(
                parsed.command, false, empty, ignored_prompt, error),
        "non-interactive confirmation token is case-sensitive");
}

void test_help_contract() {
    std::ostringstream help;
    ccs::print_config_cli_help(help);
    const auto text = help.str();
    require(text.find("ccs-trans config set <key> <value>") != std::string::npos, "config help");
    require(text.find("ccs-trans profile enable <profile>") != std::string::npos, "profile help");
    require(text.find("ccs-trans rule move <profile> <rule> <1-based-position>") != std::string::npos,
        "rule move help");
    require(text.find("ccs-trans storage migrate") != std::string::npos,
        "storage migration help");
    require(text.find("storage migrate --replace [--confirm REPLACE]")
            != std::string::npos,
        "destructive migration help documents explicit confirmation");
    require(text.find(", -h") == std::string::npos, "short help alias omitted");
    require(text.find("listener.port") != std::string::npos, "application key documented");
    require(text.find("logging.max-total-size") != std::string::npos,
        "log retention key documented");
    require(text.find("runtime.max-inflight-bytes") != std::string::npos,
        "inflight budget key documented");
    require(text.find("local.request-path") != std::string::npos, "profile key documented");
}

void test_management_workflow() {
    const auto root = unique_test_root();
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto paths = ccs::make_app_paths(root);
    ccs::ConfigStore store(paths);
    std::string error;
    require(store.load(error), error);

    auto output = execute(store, {"config", "show"});
    auto shown = nlohmann::json::parse(output);
    require(shown["listener"]["port"] == 15723, "config show uses defaults");
    require(!std::filesystem::exists(paths.config_file), "read-only show does not create config");

    output = execute(store, {"config", "set", "listener.port", "16000"});
    shown = nlohmann::json::parse(output);
    require(shown["listener"]["port"] == 16000, "typed integer config set");
    output = execute(store, {"config", "set", "logging.body", "false"});
    require(nlohmann::json::parse(output)["logging"]["body"] == false, "typed boolean config set");
    output = execute(store, {"config", "set", "logging.max-total-size", "33554432"});
    require(nlohmann::json::parse(output)["logging"]["max_total_size"] == 33554432,
        "log total size config set uses one canonical key");
    output = execute(store, {"config", "unset", "logging.max-total-size"});
    require(nlohmann::json::parse(output)["logging"]["max_total_size"] == 2147483648ULL,
        "log total size unset restores 2 GiB");
    output = execute(store, {"config", "unset", "listener.port"});
    require(nlohmann::json::parse(output)["listener"]["port"] == 15723, "config unset restores default");

    output = execute(store, {"profile", "create", "findcg"});
    shown = nlohmann::json::parse(output);
    require(shown["id"] == "findcg" && shown["enabled"] == false, "profile create makes disabled draft");
    execute(store, {"profile", "create", "backup"});
    set_complete_profile(store, "findcg", "/findcg");
    set_complete_profile(store, "backup", "/backup");

    const auto before_incomplete_enable = read_file(paths.config_file);
    execute(store, {"profile", "create", "incomplete"});
    const auto after_create = read_file(paths.config_file);
    require(execute_fails(store, {"profile", "enable", "incomplete"}, error)
            && error.find("enabled but missing") != std::string::npos,
        "incomplete profile cannot be enabled");
    require(read_file(paths.config_file) == after_create, "failed profile enable leaves bytes unchanged");
    require(before_incomplete_enable != after_create, "successful create changed bytes");
    execute(store, {"profile", "remove", "incomplete"});

    output = execute(store, {"rule", "add", "findcg", "remove-image", "remove_tool"});
    shown = nlohmann::json::parse(output);
    require(shown["enabled"] == false && shown["type"] == "remove_tool", "rule add makes disabled rule");
    const auto before_invalid_rule_enable = read_file(paths.config_file);
    require(execute_fails(store, {"rule", "enable", "findcg", "remove-image"}, error)
            && error.find("missing required option: tool") != std::string::npos,
        "rule enable performs factory option validation");
    require(read_file(paths.config_file) == before_invalid_rule_enable,
        "failed semantic rule enable leaves bytes unchanged");
    output = execute(store, {"rule", "set", "findcg", "remove-image", "tool", "image_gen"});
    require(nlohmann::json::parse(output)["tool"] == "image_gen", "bare rule value stored as string");
    output = execute(store, {"rule", "enable", "findcg", "remove-image"});
    require(nlohmann::json::parse(output)["enabled"] == true, "rule enabled");
    execute(store, {"rule", "disable", "findcg", "remove-image"});
    execute(store, {"rule", "enable", "findcg", "remove-image"});

    execute(store, {"rule", "add", "findcg", "set-model", "set_field"});
    execute(store, {"rule", "set", "findcg", "set-model", "path", "\"/model\""});
    output = execute(store, {"rule", "set", "findcg", "set-model", "value", "{\"name\":\"gpt\"}"});
    require(nlohmann::json::parse(output)["value"]["name"] == "gpt", "JSON rule value retains object type");
    output = execute(store, {"rule", "move", "findcg", "set-model", "1"});
    shown = nlohmann::json::parse(output);
    require(shown[0]["id"] == "set-model" && shown[1]["id"] == "remove-image", "rule move is 1-based");
    output = execute(store, {"rule", "show", "findcg", "set-model"});
    require(nlohmann::json::parse(output)["path"] == "/model", "rule show is typed");
    output = execute(store, {"rule", "unset", "findcg", "set-model", "value"});
    require(!nlohmann::json::parse(output).contains("value"), "rule option unset");
    output = execute(store, {"rule", "remove", "findcg", "set-model"});
    require(nlohmann::json::parse(output).size() == 1, "rule remove returns remaining list");

    execute(store, {"profile", "enable", "findcg"});
    execute(store, {"profile", "enable", "backup"});
    output = execute(store, {"profile", "list"});
    shown = nlohmann::json::parse(output);
    require(shown.size() == 2, "two same-protocol profiles configured");
    require(shown[0]["enabled"] == true && shown[1]["enabled"] == true, "both profiles enabled");

    const auto before_route_collision = read_file(paths.config_file);
    require(execute_fails(
                store,
                {"profile", "set", "backup", "local.request-path", "/findcg/v1/responses"},
                error)
            && error.find("route collision") != std::string::npos,
        "CLI rejects a canonical route collision before save");
    require(read_file(paths.config_file) == before_route_collision,
        "failed route collision leaves config bytes unchanged");

    const auto before_invalid_unset = read_file(paths.config_file);
    require(execute_fails(store, {"profile", "unset", "findcg", "local.request-path"}, error)
            && error.find("enabled but missing") != std::string::npos,
        "enabled profile cannot lose required field");
    require(read_file(paths.config_file) == before_invalid_unset, "failed profile unset leaves bytes unchanged");
    execute(store, {"profile", "disable", "findcg"});
    execute(store, {"profile", "unset", "findcg", "local.request-path"});
    execute(store, {"profile", "set", "findcg", "local.request-path", "/findcg/v1/responses"});
    execute(store, {"profile", "enable", "findcg"});

    const auto before_invalid_config = read_file(paths.config_file);
    require(execute_fails(store, {"config", "set", "runtime.worker-threads", "65"}, error)
            && error.find("max_connections") != std::string::npos,
        "cross-field application validation runs before save");
    require(read_file(paths.config_file) == before_invalid_config, "failed config set leaves bytes unchanged");
    execute(store, {"config", "set", "runtime.max-connections", "128"});
    execute(store, {"config", "set", "runtime.worker-threads", "96"});
    require(execute_fails(store, {"config", "unset", "runtime.max-connections"}, error),
        "config unset cannot create an invalid worker relation");
    execute(store, {"config", "unset", "runtime.worker-threads"});
    execute(store, {"config", "unset", "runtime.max-connections"});

    output = execute(store, {"config", "show"});
    const auto disk = nlohmann::json::parse(read_file(paths.config_file));
    shown = nlohmann::json::parse(output);
    require(shown["listener"] == disk["listener"], "config show matches disk listener");
    require(shown["runtime"] == disk["runtime"], "config show matches disk runtime");
    require(shown["logging"] == disk["logging"], "config show matches disk logging");
    output = execute(store, {"profile", "show", "findcg"});
    shown = nlohmann::json::parse(output);
    shown.erase("id");
    require(shown == disk["profiles"]["findcg"], "profile show matches disk profile");
    output = execute(store, {"rule", "show", "findcg", "remove-image"});
    require(nlohmann::json::parse(output) == disk["profiles"]["findcg"]["rules"][0],
        "rule show matches disk rule");
    ccs::ConfigStore reloaded(paths);
    require(reloaded.load(error), error);
    require(reloaded.document().profiles.size() == 2, "workflow persisted two profiles");
    require(reloaded.document().profiles.at("findcg").rules.size() == 1, "workflow persisted rule pipeline");

    ccs::ConfigCliCommand invalid_command;
    invalid_command.kind = ccs::ConfigCliCommandKind::ConfigUnset;
    invalid_command.key = "unknown";
    std::string invalid_output;
    require(!ccs::execute_config_cli(invalid_command, reloaded, invalid_output, error)
            && error.find("unknown application config key") != std::string::npos,
        "executor validates commands even when parser is bypassed");

    std::filesystem::remove_all(root, ec);
}

void test_composite_management_workflow() {
    const auto root = unique_test_root() / "composite";
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    const auto paths = ccs::make_app_paths(root);
    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(repository.load(error), error);

    auto output = execute_composite(repository, {"config", "show"});
    auto shown = nlohmann::json::parse(output);
    require(shown["runtime"]["max_inflight_bytes"] == 536870912ULL,
        "composite config show includes inflight budget");
    output = execute_composite(
        repository,
        {"config", "set", "runtime.max-inflight-bytes", "805306368"});
    require(nlohmann::json::parse(output)["runtime"]["max_inflight_bytes"]
            == 805306368ULL,
        "typed inflight budget persists through v3 config");

    execute_composite(repository, {"profile", "create", "findcg"});
    const auto original_key = repository.snapshot().profiles.front().key;
    require(original_key > 0, "composite CLI profile receives stable key");
    execute_composite(repository, {"profile", "set", "findcg", "protocol", "responses"});
    execute_composite(repository,
        {"profile", "set", "findcg", "local.request-path", "/findcg/v1/responses"});
    execute_composite(repository,
        {"profile", "set", "findcg", "local.usage-path", "/findcg/v1/usage"});
    execute_composite(repository,
        {"profile", "set", "findcg", "upstream.base-url", "https://example.test/v1"});
    execute_composite(repository,
        {"profile", "set", "findcg", "upstream.request-path", "/responses"});
    execute_composite(repository,
        {"profile", "set", "findcg", "upstream.usage-path", "/usage"});
    execute_composite(repository, {"profile", "rename", "findcg", "primary"});
    require(repository.snapshot().profiles.front().profile_id == "primary",
        "composite CLI renames profile");
    require(repository.snapshot().profiles.front().key == original_key,
        "profile rename preserves stable key");

    execute_composite(repository,
        {"rule", "add", "primary", "remove-image", "remove_tool"});
    const auto rule_key = repository.snapshot().profiles.front().rules.front().key;
    execute_composite(repository,
        {"rule", "set", "primary", "remove-image", "tool", "image_gen"});
    execute_composite(repository, {"rule", "enable", "primary", "remove-image"});
    require(repository.snapshot().profiles.front().rules.front().key == rule_key,
        "composite CLI rule edits preserve stable key");

    execute_composite(repository, {"profile", "create", "secondary"});
    output = execute_composite(repository, {"profile", "move", "secondary", "1"});
    shown = nlohmann::json::parse(output);
    require(shown[0]["id"] == "secondary" && shown[1]["id"] == "primary",
        "composite CLI preserves explicit profile order");

    ccs::CompositeConfigRepository reloaded(paths);
    require(reloaded.load(error), error);
    require(reloaded.snapshot().application.runtime.max_inflight_bytes == 805306368ULL,
        "composite CLI application field round-trips");
    require(reloaded.snapshot().profiles[1].key == original_key,
        "composite CLI stable profile key round-trips");
    require(reloaded.snapshot().profiles[1].rules.front().key == rule_key,
        "composite CLI stable rule key round-trips");
    require(reloaded.verify_storage(error), error);

    std::filesystem::remove_all(root, ec);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"parser contract", test_parser_contract},
        {"storage replacement confirmation", test_storage_replacement_confirmation},
        {"help contract", test_help_contract},
        {"management workflow", test_management_workflow},
        {"composite management workflow", test_composite_management_workflow},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "ok: " << name << "\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "config CLI tests failed: " << ex.what() << "\n";
        return 1;
    }
}
