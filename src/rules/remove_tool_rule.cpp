#include "rules/remove_tool_rule.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace ccs {

namespace {

constexpr std::size_t kMaxToolNameBytes = 256;

enum class ToolLayout {
    Responses,
    Chat,
    Messages,
};

bool contains_control(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch < 0x20 || ch == 0x7F;
    });
}

bool string_field_equals(
    const nlohmann::json& value,
    const char* name,
    const std::string& expected) {
    const auto field = value.find(name);
    if (field == value.end() || !field->is_string()) {
        return false;
    }
    return field->get_ref<const std::string&>() == expected;
}

bool matches_tool(
    const nlohmann::json& candidate,
    ToolLayout layout,
    const std::string& tool_name) {
    if (!candidate.is_object()) {
        return false;
    }
    switch (layout) {
    case ToolLayout::Responses:
        return string_field_equals(candidate, "name", tool_name)
            || string_field_equals(candidate, "namespace", tool_name);
    case ToolLayout::Chat: {
        const auto function = candidate.find("function");
        return function != candidate.end()
            && function->is_object()
            && string_field_equals(*function, "name", tool_name);
    }
    case ToolLayout::Messages:
        return string_field_equals(candidate, "name", tool_name);
    }
    return false;
}

class RemoveToolRule final : public CompiledRule {
public:
    RemoveToolRule(std::string id, ToolLayout layout, std::string tool_name)
        : CompiledRule(std::move(id), "remove_tool")
        , layout_(layout)
        , tool_name_(std::move(tool_name)) {}

    RuleApplyResult apply(nlohmann::json& body) const override {
        if (!body.is_object()) {
            throw RuleRuntimeError(
                "invalid_root_type",
                "request body must be a JSON object for remove_tool");
        }
        const auto tools = body.find("tools");
        if (tools == body.end()) {
            return {false, false, "tools_absent", {"/tools", 0}};
        }
        if (!tools->is_array()) {
            return {false, false, "tools_not_array", {"/tools", 0}};
        }

        auto& tool_array = tools->get_ref<nlohmann::json::array_t&>();
        std::size_t removed = 0;
        const auto retained_end = std::remove_if(
            tool_array.begin(),
            tool_array.end(),
            [&](const nlohmann::json& tool) {
                if (!matches_tool(tool, layout_, tool_name_)) {
                    return false;
                }
                ++removed;
                return true;
            });
        if (removed == 0) {
            return {false, false, "tool_not_found", {"/tools", 0}};
        }
        tool_array.erase(retained_end, tool_array.end());
        return {true, true, "tool_removed", {"/tools", removed}};
    }

private:
    ToolLayout layout_;
    std::string tool_name_;
};

class RemoveToolRuleFactory final : public RuleFactory {
public:
    std::string_view type() const noexcept override {
        return "remove_tool";
    }

    const RuleDescriptor& descriptor() const noexcept override {
        static const RuleDescriptor value{
            "remove_tool",
            "rule.remove_tool",
            true,
            {
                {"tool", "rule.option.tool", RuleOptionValueType::String, true, 0},
            },
        };
        return value;
    }

    bool compile(
        const RuleDefinition& definition,
        const ProtocolHandler& protocol,
        std::shared_ptr<const CompiledRule>& rule,
        std::string& error) const override {
        error.clear();
        if (!protocol.descriptor().request_body_is_json) {
            error = "protocol " + std::string(protocol.id())
                + " does not declare a JSON request body";
            return false;
        }
        if (!protocol.supports_specialized_rule("remove_tool")) {
            error = "remove_tool is not supported by protocol " + std::string(protocol.id());
            return false;
        }
        if (definition.options.size() != 1 || definition.options.count("tool") == 0) {
            for (const auto& [key, value] : definition.options) {
                (void)value;
                if (key != "tool") {
                    error = "unsupported option: " + key;
                    return false;
                }
            }
            error = "missing required option: tool";
            return false;
        }
        const auto& tool = definition.options.at("tool");
        if (!tool.is_string()) {
            error = "option tool must be a string";
            return false;
        }
        const auto& tool_name = tool.get_ref<const std::string&>();
        if (tool_name.empty()
            || tool_name.size() > kMaxToolNameBytes
            || contains_control(tool_name)) {
            error = "option tool must be 1 to 256 bytes without control characters";
            return false;
        }

        ToolLayout layout;
        if (protocol.id() == "responses") {
            layout = ToolLayout::Responses;
        } else if (protocol.id() == "chat") {
            layout = ToolLayout::Chat;
        } else if (protocol.id() == "messages") {
            layout = ToolLayout::Messages;
        } else {
            error = "remove_tool has no implementation for protocol "
                + std::string(protocol.id());
            return false;
        }
        rule = std::make_shared<const RemoveToolRule>(
            definition.id.value,
            layout,
            tool_name);
        return true;
    }
};

} // namespace

std::shared_ptr<const RuleFactory> make_remove_tool_rule_factory() {
    return std::make_shared<const RemoveToolRuleFactory>();
}

} // namespace ccs
