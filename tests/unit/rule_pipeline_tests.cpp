#include "config/config_document.hpp"
#include "config/runtime_compiler.hpp"
#include "protocols/protocol_handler.hpp"
#include "protocols/protocol_registry.hpp"
#include "rules/generic_json_rules.hpp"
#include "rules/rule.hpp"
#include "rules/rule_registry.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ccs::RuleDefinition rule(
    std::string id,
    std::string type,
    std::map<std::string, nlohmann::json> options = {}) {
    ccs::RuleDefinition definition;
    definition.id.value = std::move(id);
    definition.enabled = true;
    definition.type = std::move(type);
    definition.options = std::move(options);
    return definition;
}

std::shared_ptr<const ccs::ProtocolHandler> protocol(const std::string& id) {
    const auto handler = ccs::builtin_protocol_registry()->find(id);
    require(handler != nullptr, "missing builtin protocol: " + id);
    return handler;
}

std::shared_ptr<const ccs::CompiledPipeline> compile(
    const std::vector<ccs::RuleDefinition>& definitions,
    const std::string& protocol_id = "responses") {
    std::shared_ptr<const ccs::CompiledPipeline> pipeline;
    std::string error;
    require(ccs::builtin_rule_registry()->compile_pipeline(
                definitions,
                protocol(protocol_id),
                pipeline,
                error),
        error);
    return pipeline;
}

bool compile_fails(
    const std::vector<ccs::RuleDefinition>& definitions,
    const std::shared_ptr<const ccs::ProtocolHandler>& handler,
    const std::string& expected) {
    std::shared_ptr<const ccs::CompiledPipeline> pipeline;
    std::string error;
    return !ccs::builtin_rule_registry()->compile_pipeline(
               definitions,
               handler,
               pipeline,
               error)
        && error.find(expected) != std::string::npos;
}

class SyntheticHandler final : public ccs::ProtocolHandler {
public:
    explicit SyntheticHandler(ccs::ProtocolDescriptor descriptor)
        : descriptor_(std::move(descriptor)) {}

    const ccs::ProtocolDescriptor& descriptor() const noexcept override {
        return descriptor_;
    }

private:
    ccs::ProtocolDescriptor descriptor_;
};

class ThrowingRule final : public ccs::CompiledRule {
public:
    ThrowingRule()
        : CompiledRule("throws", "test_rule") {}

    ccs::RuleApplyResult apply(nlohmann::json&) const override {
        throw std::runtime_error("sensitive implementation detail");
    }
};

class NoopRule final : public ccs::CompiledRule {
public:
    explicit NoopRule(std::string id)
        : CompiledRule(std::move(id), "custom_rule") {}

    ccs::RuleApplyResult apply(nlohmann::json&) const override {
        return {false, false, "noop", {"", 0}};
    }
};

class NoopFactory final : public ccs::RuleFactory {
public:
    std::string_view type() const noexcept override {
        return "custom_rule";
    }

    bool compile(
        const ccs::RuleDefinition& definition,
        const ccs::ProtocolHandler&,
        std::shared_ptr<const ccs::CompiledRule>& compiled,
        std::string& error) const override {
        error.clear();
        if (!definition.options.empty()) {
            error = "custom_rule does not accept options";
            return false;
        }
        compiled = std::make_shared<const NoopRule>(definition.id.value);
        return true;
    }
};

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "failed to open fixture: " + path.string());
    std::ostringstream content;
    content << input.rdbuf();
    return content.str();
}

void test_registry_and_compile_validation() {
    const auto registry = ccs::builtin_rule_registry();
    require(registry->rule_types()
            == std::vector<std::string>({"remove_field", "remove_tool", "set_field"}),
        "builtin rule types are stable and sorted");

    auto mutable_registry = std::make_shared<ccs::RuleRegistry>(*registry);
    std::string error;
    require(!mutable_registry->register_factory(nullptr, error)
            && error.find("null") != std::string::npos,
        "null factory rejected");
    require(!mutable_registry->register_factory(ccs::make_set_field_rule_factory(), error)
            && error.find("already registered") != std::string::npos,
        "duplicate factory rejected");

    const auto unknown = rule("unknown", "future_rule");
    require(compile_fails({unknown}, protocol("responses"), "unknown type"),
        "unknown enabled rule rejected");
    auto disabled_unknown = unknown;
    disabled_unknown.enabled = false;
    const auto empty = compile({disabled_unknown});
    require(empty && empty->empty(), "disabled unknown draft rule remains uncompiled");

    auto duplicate = disabled_unknown;
    duplicate.enabled = true;
    duplicate.type = "remove_field";
    duplicate.options["path"] = "/field";
    require(compile_fails({disabled_unknown, duplicate}, protocol("responses"), "duplicate rule id"),
        "duplicate ids rejected independently of enabled state");
    require(compile_fails({rule("set", "set_field", {{"path", "/x"}})},
                protocol("responses"),
                "missing required option: value"),
        "missing set_field value rejected");
    require(compile_fails({rule("set", "set_field", {
                                   {"path", "/x"},
                                   {"value", 1},
                                   {"secret", "must-not-log"},
                               })},
                protocol("responses"),
                "unsupported option: secret"),
        "unknown option rejected without echoing its value");
    require(compile_fails({rule("remove-root", "remove_field", {{"path", ""}})},
                protocol("responses"),
                "cannot remove"),
        "remove_field root target rejected at compile time");
    require(compile_fails({rule("bad-tool", "remove_tool", {{"tool", 7}})},
                protocol("responses"),
                "tool must be a string"),
        "remove_tool enforces its typed option");
    require(compile_fails({rule("extra-tool", "remove_tool", {
                                   {"tool", "image_gen"},
                                   {"scope", "root"},
                               })},
                protocol("responses"),
                "unsupported option: scope"),
        "remove_tool rejects unknown options");
    require(compile_fails({rule("bad-pointer", "set_field", {
                                   {"path", "/bad~2escape"},
                                   {"value", 1},
                               })},
                protocol("responses"),
                "invalid RFC 6901 escape"),
        "invalid pointer escape rejected at compile time");

    ccs::ProtocolDescriptor opaque_descriptor;
    opaque_descriptor.id = "opaque";
    opaque_descriptor.request_method = "POST";
    opaque_descriptor.request_body_is_json = false;
    auto opaque = std::make_shared<SyntheticHandler>(std::move(opaque_descriptor));
    require(compile_fails({rule("set", "set_field", {
                                   {"path", "/x"},
                                   {"value", 1},
                               })},
                opaque,
                "does not declare a JSON request body"),
        "generic JSON rule rejects opaque protocols");

    ccs::ProtocolDescriptor no_tools_descriptor;
    no_tools_descriptor.id = "json_no_tools";
    no_tools_descriptor.request_method = "POST";
    no_tools_descriptor.request_body_is_json = true;
    auto no_tools = std::make_shared<SyntheticHandler>(std::move(no_tools_descriptor));
    require(compile_fails({rule("remove", "remove_tool", {{"tool", "image_gen"}})},
                no_tools,
                "not supported by protocol"),
        "specialized rule capability enforced by the factory");

    std::shared_ptr<const ccs::CompiledPipeline> untouched = compile({});
    require(!registry->compile_pipeline({}, nullptr, untouched, error)
            && error.find("protocol handler") != std::string::npos
            && untouched != nullptr,
        "failed compile leaves output pipeline unchanged");
}

void test_runtime_compiler_rule_registry_snapshot() {
    auto rules = std::make_shared<ccs::RuleRegistry>(*ccs::builtin_rule_registry());
    ccs::RuntimeCompiler compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-rule-registry-test",
        ccs::builtin_protocol_registry(),
        rules);

    std::string error;
    require(rules->register_factory(std::make_shared<const NoopFactory>(), error), error);
    auto document = ccs::make_default_config_document();
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = "/custom/request";
    profile.upstream.base_url = "https://example.com";
    profile.upstream.request_path = "/v1/responses";
    profile.rules.push_back(rule("custom", "custom_rule"));
    document.profiles.emplace("custom", std::move(profile));

    ccs::RuntimeSnapshotPtr snapshot;
    require(!compiler.compile(document, {}, snapshot, error)
            && error.find("unknown type") != std::string::npos,
        "compiler does not observe factories registered after construction");
    ccs::RuntimeCompiler refreshed(
        std::filesystem::temp_directory_path() / "ccs-trans-rule-registry-test",
        ccs::builtin_protocol_registry(),
        rules);
    require(refreshed.compile(document, {}, snapshot, error), error);
    require(snapshot->rules
            && snapshot->rules->find("custom_rule")
            && snapshot->profiles.at("custom")->request_pipeline->size() == 1,
        "refreshed compiler publishes its immutable rule registry generation");

    auto invalid_compiler = ccs::RuntimeCompiler(
        std::filesystem::temp_directory_path() / "ccs-trans-rule-registry-test",
        ccs::builtin_protocol_registry(),
        nullptr);
    const auto previous = snapshot;
    require(!invalid_compiler.compile(document, {}, snapshot, error)
            && error.find("requires a rule registry") != std::string::npos
            && snapshot == previous,
        "missing registry fails without replacing the previous snapshot");
}

void test_pipeline_parse_order_and_rollback() {
    const auto empty = compile({});
    const std::string invalid = "not json";
    const auto transparent = empty->apply(invalid);
    require(transparent.ok
            && transparent.parse_count == 0
            && transparent.serialize_count == 0
            && !transparent.modified
            && !transparent.rewritten_body,
        "empty pipeline reuses non-JSON bytes without parsing");

    const auto ordered = compile({
        rule("first", "set_field", {{"path", "/model"}, {"value", "middle"}}),
        rule("second", "set_field", {{"path", "/model"}, {"value", "final"}}),
    });
    const auto transformed = ordered->apply(R"({"model":"initial","input":"hi"})");
    require(transformed.ok
            && transformed.modified
            && transformed.parse_count == 1
            && transformed.serialize_count == 1
            && transformed.traces.size() == 2,
        "non-empty modified pipeline parses and serializes once");
    require(transformed.traces[0].rule_id == "first"
            && transformed.traces[1].rule_id == "second",
        "rule order retained in execution traces");
    require(nlohmann::json::parse(*transformed.rewritten_body)["model"] == "final",
        "later rule sees and overrides the shared DOM");

    const auto unchanged = compile({
        rule("same", "set_field", {{"path", "/model"}, {"value", "initial"}}),
    })->apply(R"({ "model": "initial" })");
    require(unchanged.ok
            && !unchanged.modified
            && unchanged.parse_count == 1
            && unchanged.serialize_count == 0
            && !unchanged.rewritten_body
            && unchanged.traces[0].matched
            && !unchanged.traces[0].modified,
        "matched unchanged rule preserves the original byte representation");

    const std::string original = R"({"model":"initial"})";
    const auto transactional = compile({
        rule("change", "set_field", {{"path", "/model"}, {"value", "changed"}}),
        rule("fail", "remove_field", {{"path", "/missing"}}),
    })->apply(original);
    require(!transactional.ok
            && !transactional.modified
            && !transactional.rewritten_body
            && transactional.output_body_size == original.size()
            && transactional.serialize_count == 0,
        "runtime failure rolls back partial DOM changes");
    require(transactional.error
            && transactional.error->status_code == 400
            && transactional.error->rule_id == "fail"
            && transactional.error->reason == "target_missing",
        "runtime rule error has bounded actionable context");
    require(transactional.traces.size() == 2
            && transactional.traces[0].modified
            && !transactional.traces[1].modified,
        "rollback trace retains execution history without publishing a body");

    const auto invalid_json = ordered->apply("not json");
    require(!invalid_json.ok
            && invalid_json.parse_count == 1
            && invalid_json.serialize_count == 0
            && invalid_json.traces.empty()
            && invalid_json.error
            && invalid_json.error->reason == "invalid_json",
        "invalid JSON fails before any rule executes");

    const ccs::CompiledPipeline broken({std::make_shared<const ThrowingRule>()});
    const auto internal_error = broken.apply(R"({"value":1})");
    require(!internal_error.ok
            && internal_error.error
            && internal_error.error->status_code == 500
            && internal_error.error->reason == "unexpected_rule_error"
            && internal_error.error->message.find("sensitive") == std::string::npos,
        "unexpected rule errors are sanitized and classified as server errors");
}

void test_json_pointer_semantics() {
    const auto escaped = compile({
        rule("escaped", "set_field", {{"path", "/a~1b/~0key"}, {"value", 2}}),
        rule("array", "remove_field", {{"path", "/items/0"}}),
    })->apply(R"({"a/b":{"~key":1},"items":["first","second"]})");
    require(escaped.ok && escaped.modified, "escaped JSON Pointer pipeline succeeds");
    const auto escaped_body = nlohmann::json::parse(*escaped.rewritten_body);
    require(escaped_body["a/b"]["~key"] == 2
            && escaped_body["items"] == nlohmann::json::array({"second"}),
        "RFC 6901 escaping and array removal are exact");

    const auto root = compile({
        rule("root", "set_field", {{"path", ""}, {"value", nlohmann::json::array({1, 2})}}),
    })->apply(R"({"old":true})");
    require(root.ok
            && root.modified
            && nlohmann::json::parse(*root.rewritten_body) == nlohmann::json::array({1, 2}),
        "empty pointer can replace the document root");

    const std::vector<std::pair<std::string, std::string>> failures = {
        {"/missing", "target_missing"},
        {"/scalar/child", "type_conflict"},
        {"/items/01", "invalid_array_index"},
        {"/items/-", "invalid_array_index"},
        {"/items/2", "array_index_out_of_bounds"},
    };
    const std::string body = R"({"scalar":1,"items":["only"]})";
    for (const auto& [path, reason] : failures) {
        const auto result = compile({
            rule("strict", "set_field", {{"path", path}, {"value", 2}}),
        })->apply(body);
        require(!result.ok
                && result.error
                && result.error->reason == reason
                && !result.modified
                && !result.rewritten_body,
            "strict JSON Pointer failure: " + path);
    }

    const std::string long_key(300, 'k');
    nlohmann::json long_body = {{long_key, "before"}};
    const auto bounded = compile({
        rule("bounded", "set_field", {{"path", "/" + long_key}, {"value", "secret-value"}}),
    })->apply(long_body.dump());
    require(bounded.ok
            && bounded.traces[0].summary.target.size() <= 64
            && bounded.traces[0].summary.target.find("secret-value") == std::string::npos
            && bounded.traces[0].reason.find("secret-value") == std::string::npos,
        "rule trace bounds long paths and never records replacement values");
}

void test_remove_tool_protocol_layouts() {
    const auto responses = compile({
        rule("remove-image", "remove_tool", {{"tool", "image_gen"}}),
    }, "responses");
    const auto responses_result = responses->apply(
        R"({"tools":[{"name":"image_gen"},{"namespace":"image_gen"},{"name":"web_search"}],"nested":{"tools":[{"name":"image_gen"}]}})");
    require(responses_result.ok
            && responses_result.modified
            && responses_result.traces[0].summary.affected_count == 2,
        "Responses removes root name and namespace matches");
    const auto responses_body = nlohmann::json::parse(*responses_result.rewritten_body);
    require(responses_body["tools"].size() == 1
            && responses_body["tools"][0]["name"] == "web_search"
            && responses_body["nested"]["tools"].size() == 1,
        "Responses preserves order and does not scan nested tools");

    const auto chat = compile({
        rule("remove-image", "remove_tool", {{"tool", "image_gen"}}),
    }, "chat");
    const auto chat_result = chat->apply(
        R"({"tools":[{"type":"function","function":{"name":"image_gen"}},{"name":"image_gen"},{"type":"function","function":{"name":"web_search"}}]})");
    const auto chat_body = nlohmann::json::parse(*chat_result.rewritten_body);
    require(chat_result.ok
            && chat_result.traces[0].summary.affected_count == 1
            && chat_body["tools"].size() == 2
            && chat_body["tools"][0]["name"] == "image_gen",
        "Chat removes only function.name matches");

    const auto messages = compile({
        rule("remove-image", "remove_tool", {{"tool", "image_gen"}}),
    }, "messages");
    const auto messages_result = messages->apply(
        R"({"tools":[{"name":"image_gen","input_schema":{}},{"name":"other"}]})");
    const auto messages_body = nlohmann::json::parse(*messages_result.rewritten_body);
    require(messages_result.ok
            && messages_result.traces[0].summary.affected_count == 1
            && messages_body["tools"].size() == 1
            && messages_body["tools"][0]["name"] == "other",
        "Messages removes root name matches");

    for (const auto& body : {R"({"input":"hi"})", R"({"tools":"image_gen"})"}) {
        const auto transparent = responses->apply(body);
        require(transparent.ok
                && !transparent.modified
                && transparent.parse_count == 1
                && transparent.serialize_count == 0
                && !transparent.rewritten_body,
            "missing or non-array tools stays byte-transparent");
    }
    const auto invalid_root = responses->apply(R"([{"name":"image_gen"}])");
    require(!invalid_root.ok
            && invalid_root.error
            && invalid_root.error->status_code == 400
            && invalid_root.error->reason == "invalid_root_type",
        "remove_tool requires an object root");

    const auto remove_all = responses->apply(R"({"tools":[{"name":"image_gen"}]})");
    require(remove_all.ok
            && nlohmann::json::parse(*remove_all.rewritten_body)["tools"].empty(),
        "removing every match leaves an explicit empty tools array");
}

void test_findcg_fixture_equivalence() {
    const auto fixture_path = std::filesystem::path(CCS_TRANS_TEST_FIXTURE_DIR)
        / "stage11/findcg-transform-cases.json";
    const auto fixture = nlohmann::json::parse(read_file(fixture_path));
    const auto pipeline = compile({
        rule("remove-image-gen", "remove_tool", {{"tool", "image_gen"}}),
    });

    for (const auto& test_case : fixture.at("cases")) {
        const auto name = test_case.at("name").get<std::string>();
        if (name == "non-findcg-invalid-json-remains-transparent") {
            continue;
        }
        const auto result = pipeline->apply(test_case.at("body").get<std::string>());
        if (test_case.contains("expected_error_status")) {
            require(!result.ok
                    && result.error
                    && result.error->status_code == test_case.at("expected_error_status"),
                "fixture error behavior: " + name);
            continue;
        }
        const auto& expected = test_case.at("expected");
        require(result.ok
                && result.modified == expected.at("modified").get<bool>()
                && result.traces.size() == 1
                && result.traces[0].summary.affected_count
                    == expected.at("removed_tools_count").get<std::size_t>(),
            "fixture transform behavior: " + name);
        if (result.modified) {
            require(result.rewritten_body
                    && nlohmann::json::parse(*result.rewritten_body) == expected.at("body"),
                "fixture transformed body: " + name);
        } else {
            require(!result.rewritten_body && result.serialize_count == 0,
                "fixture unchanged bytes reused: " + name);
        }
    }
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"registry and compile validation", test_registry_and_compile_validation},
        {"runtime compiler rule registry snapshot", test_runtime_compiler_rule_registry_snapshot},
        {"pipeline parse order and rollback", test_pipeline_parse_order_and_rollback},
        {"JSON Pointer semantics", test_json_pointer_semantics},
        {"remove_tool protocol layouts", test_remove_tool_protocol_layouts},
        {"findcg fixture equivalence", test_findcg_fixture_equivalence},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "ok: " << name << "\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "rule pipeline tests failed: " << ex.what() << "\n";
        return 1;
    }
}
