#include "config/config_document.hpp"
#include "config/runtime_compiler.hpp"
#include "protocols/chat_handler.hpp"
#include "protocols/messages_handler.hpp"
#include "protocols/protocol_handler.hpp"
#include "protocols/protocol_registry.hpp"
#include "protocols/responses_handler.hpp"
#include "routing/route_table.hpp"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
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

ccs::ProtocolDescriptor synthetic_descriptor(
    std::string id = "echo",
    std::string request_method = "PUT") {
    ccs::ProtocolDescriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.request_method = std::move(request_method);
    descriptor.supports_usage = false;
    descriptor.supports_sse = false;
    descriptor.request_body_is_json = false;
    descriptor.error_envelope = ccs::ProtocolErrorEnvelope::OpenAI;
    return descriptor;
}

ccs::ProfileDefinition complete_profile(
    const std::string& protocol,
    bool usage = false) {
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{protocol};
    profile.local.request_path = "/echo/request";
    profile.upstream.base_url = "https://echo.example.com";
    profile.upstream.request_path = "/v1/request";
    if (usage) {
        profile.local.usage_path = "/echo/usage";
        profile.upstream.usage_path = "/v1/usage";
    }
    return profile;
}

std::shared_ptr<const ccs::ProtocolRegistry> immutable(
    const std::shared_ptr<ccs::ProtocolRegistry>& registry) {
    return registry;
}

void test_builtin_descriptors() {
    const auto registry = ccs::builtin_protocol_registry();
    require(registry != nullptr, "builtin registry exists");
    require(registry->protocol_ids() == std::vector<std::string>({"chat", "messages", "responses"}),
        "builtin protocol ids are stable and sorted");

    for (const auto& id : {"responses", "chat", "messages"}) {
        const auto handler = registry->find(id);
        require(handler != nullptr && handler->id() == id, std::string("handler registered: ") + id);
        const auto& descriptor = handler->descriptor();
        require(descriptor.request_method == "POST", "builtin request method");
        require(descriptor.usage_method == "GET" && descriptor.supports_usage, "builtin Usage capability");
        require(descriptor.supports_sse, "builtin SSE capability");
        require(descriptor.request_body_is_json, "builtin JSON rule capability");
        require(handler->supports_specialized_rule("remove_tool"), "remove_tool capability declared");
    }
    require(!registry->find("unknown"), "unknown protocol lookup returns null");
    require(registry->is_known_specialized_rule("remove_tool"), "specialized rule union compiled");
    require(!registry->is_known_specialized_rule("set_field"), "generic rule is not protocol-specialized");
}

void test_local_error_envelopes() {
    ccs::ResponsesHandler responses;
    const auto openai = responses.local_error(400, "invalid_request_error", "bad \"field\"");
    require(openai.status_code == 400 && openai.reason == "Bad Request", "OpenAI local error status");
    require(openai.headers == ccs::Headers({{"Content-Type", "application/json"}}),
        "OpenAI local error content type");
    const auto openai_body = nlohmann::json::parse(openai.body);
    require(openai_body["error"]["type"] == "invalid_request_error", "OpenAI error type");
    require(openai_body["error"]["message"] == "bad \"field\"", "OpenAI error message safely escaped");
    require(!openai_body.contains("type"), "OpenAI envelope has no Anthropic root type");

    ccs::MessagesHandler messages;
    const auto anthropic = messages.local_error(405, "invalid_request_error", "method not allowed");
    require(anthropic.status_code == 405 && anthropic.reason == "Method Not Allowed",
        "Messages local error status");
    const auto anthropic_body = nlohmann::json::parse(anthropic.body);
    require(anthropic_body["type"] == "error", "Anthropic root error type");
    require(anthropic_body["error"]["type"] == "invalid_request_error", "Anthropic error type");
    require(anthropic_body["error"]["message"] == "method not allowed", "Anthropic error message");
}

void test_registry_validation() {
    auto registry = std::make_shared<ccs::ProtocolRegistry>();
    std::string error;
    require(!registry->register_handler(nullptr, error) && error.find("null") != std::string::npos,
        "null handler rejected");
    auto echo = std::make_shared<SyntheticHandler>(synthetic_descriptor());
    require(registry->register_handler(echo, error), error);
    require(!registry->register_handler(echo, error) && error.find("already registered") != std::string::npos,
        "duplicate protocol rejected");

    auto invalid_id = std::make_shared<SyntheticHandler>(synthetic_descriptor("Bad Id"));
    require(!registry->register_handler(invalid_id, error) && error.find("invalid protocol id") != std::string::npos,
        "invalid protocol id rejected");
    auto invalid_method = std::make_shared<SyntheticHandler>(synthetic_descriptor("bad_method", "post"));
    require(!registry->register_handler(invalid_method, error) && error.find("request method") != std::string::npos,
        "invalid request method rejected");

    auto bad_usage_descriptor = synthetic_descriptor("bad_usage");
    bad_usage_descriptor.supports_usage = true;
    bad_usage_descriptor.usage_method = "get";
    require(!registry->register_handler(
                std::make_shared<SyntheticHandler>(std::move(bad_usage_descriptor)), error)
            && error.find("Usage method") != std::string::npos,
        "invalid Usage method rejected");

    auto duplicate_rule_descriptor = synthetic_descriptor("duplicate_rule");
    duplicate_rule_descriptor.specialized_rule_types = {"remove_tool", "remove_tool"};
    require(!registry->register_handler(
                std::make_shared<SyntheticHandler>(std::move(duplicate_rule_descriptor)), error)
            && error.find("duplicate specialized") != std::string::npos,
        "duplicate capability rejected");

    const auto external = std::make_shared<SyntheticHandler>(synthetic_descriptor("external"));
    auto profile = complete_profile("external");
    require(!registry->validate_profile(external, "external-profile", profile, error)
            && error.find("not registered") != std::string::npos,
        "registry validates only its own handlers");
}

void test_custom_protocol_compilation() {
    auto registry = std::make_shared<ccs::ProtocolRegistry>();
    std::string error;
    auto echo = std::make_shared<SyntheticHandler>(synthetic_descriptor());
    require(registry->register_handler(echo, error), error);

    auto document = ccs::make_default_config_document();
    document.profiles.emplace("echo-profile", complete_profile("echo"));
    ccs::RuntimeCompiler compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-protocol-test",
        immutable(registry));
    ccs::RuntimeSnapshotPtr snapshot;
    require(compiler.compile(document, {}, snapshot, error), error);
    const auto matched = snapshot->routes.lookup("PUT", "/echo/request");
    require(matched.status == ccs::RouteLookupStatus::Matched, "custom method route compiled");
    require(matched.entry->profile->handler == echo, "custom handler retained by snapshot");
    require(snapshot->routes.lookup("POST", "/echo/request").status
            == ccs::RouteLookupStatus::MethodNotAllowed,
        "custom method controls routing without router edits");
    require(snapshot->routes.size() == 1, "custom protocol without Usage compiles one route");

    auto late = std::make_shared<SyntheticHandler>(synthetic_descriptor("late", "PATCH"));
    require(registry->register_handler(late, error), error);
    auto late_document = ccs::make_default_config_document();
    late_document.profiles.emplace("late-profile", complete_profile("late"));
    require(!compiler.compile(late_document, {}, snapshot, error)
            && error.find("unknown protocol") != std::string::npos,
        "compiler captures an immutable registry snapshot");
    ccs::RuntimeCompiler refreshed_compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-protocol-test",
        immutable(registry));
    require(refreshed_compiler.compile(late_document, {}, snapshot, error), error);
    require(snapshot->routes.lookup("PATCH", "/echo/request").status
            == ccs::RouteLookupStatus::Matched,
        "new compiler sees later explicit registration");

    auto usage_document = ccs::make_default_config_document();
    usage_document.profiles.emplace("echo-profile", complete_profile("echo", true));
    require(!compiler.compile(usage_document, {}, snapshot, error)
            && error.find("does not support a Usage route") != std::string::npos,
        "handler rejects unsupported Usage route");

    auto unknown = ccs::make_default_config_document();
    unknown.profiles.emplace("unknown-profile", complete_profile("unknown"));
    require(!compiler.compile(unknown, {}, snapshot, error)
            && error.find("unknown protocol") != std::string::npos,
        "unknown protocol rejected before snapshot publication");
}

void test_specialized_rule_applicability() {
    auto registry = std::make_shared<ccs::ProtocolRegistry>();
    std::string error;
    auto echo = std::make_shared<SyntheticHandler>(synthetic_descriptor());
    require(registry->register_handler(echo, error), error);
    require(registry->register_handler(std::make_shared<ccs::ResponsesHandler>(), error), error);

    auto document = ccs::make_default_config_document();
    auto profile = complete_profile("echo");
    ccs::RuleDefinition specialized;
    specialized.id.value = "remove-image";
    specialized.enabled = true;
    specialized.type = "remove_tool";
    specialized.options["tool"] = "image_gen";
    profile.rules.push_back(specialized);
    document.profiles.emplace("echo-profile", profile);

    ccs::RuntimeCompiler compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-protocol-test",
        immutable(registry));
    ccs::RuntimeSnapshotPtr snapshot;
    require(!compiler.compile(document, {}, snapshot, error)
            && error.find("not supported by protocol echo") != std::string::npos,
        "known specialized rule rejected for unsupported protocol");

    document.profiles.at("echo-profile").rules[0].type = "set_field";
    document.profiles.at("echo-profile").rules[0].options.clear();
    document.profiles.at("echo-profile").rules[0].options["path"] = "/model";
    document.profiles.at("echo-profile").rules[0].options["value"] = "rewritten";
    require(!compiler.compile(document, {}, snapshot, error)
            && error.find("does not declare a JSON request body") != std::string::npos,
        "generic JSON rule rejects a non-JSON protocol");

    auto json_registry = std::make_shared<ccs::ProtocolRegistry>();
    auto json_descriptor = synthetic_descriptor("json_echo");
    json_descriptor.request_body_is_json = true;
    require(json_registry->register_handler(
                std::make_shared<SyntheticHandler>(std::move(json_descriptor)), error),
        error);
    document.profiles.at("echo-profile").protocol = ccs::ProtocolId{"json_echo"};
    ccs::RuntimeCompiler json_compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-protocol-test",
        immutable(json_registry));
    require(json_compiler.compile(document, {}, snapshot, error), error);
    require(snapshot->profiles.at("echo-profile")->request_pipeline
            && snapshot->profiles.at("echo-profile")->request_pipeline->size() == 1,
        "generic JSON rule compiles without a protocol-specific router branch");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"builtin descriptors", test_builtin_descriptors},
        {"local error envelopes", test_local_error_envelopes},
        {"registry validation", test_registry_validation},
        {"custom protocol compilation", test_custom_protocol_compilation},
        {"specialized rule applicability", test_specialized_rule_applicability},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "ok: " << name << "\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "protocol tests failed: " << ex.what() << "\n";
        return 1;
    }
}
