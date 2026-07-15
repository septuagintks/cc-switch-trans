#include "rules/rule_registry.hpp"

#include "rules/generic_json_rules.hpp"
#include "rules/remove_tool_rule.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ccs {

namespace {

bool valid_descriptor_key(std::string_view value, std::size_t maximum) {
    if (value.empty() || value.size() > maximum) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::islower(ch) != 0 || std::isdigit(ch) != 0
            || ch == '_' || ch == '-' || ch == '.';
    });
}

bool valid_option_value_type(RuleOptionValueType type) {
    switch (type) {
    case RuleOptionValueType::String:
    case RuleOptionValueType::JsonValue:
    case RuleOptionValueType::JsonPointer:
        return true;
    }
    return false;
}

bool validate_descriptor(
    const RuleDescriptor& descriptor,
    std::string_view factory_type,
    std::string& error) {
    if (descriptor.type != factory_type) {
        error = "rule descriptor type does not match factory type: "
            + std::string(factory_type);
        return false;
    }
    if (!valid_descriptor_key(descriptor.display_name_key, 128)) {
        error = "rule descriptor has an invalid display name key: "
            + descriptor.type;
        return false;
    }
    std::unordered_set<std::string> option_names;
    for (std::size_t index = 0; index < descriptor.options.size(); ++index) {
        const auto& option = descriptor.options[index];
        if (!valid_descriptor_key(option.name, 64)) {
            error = "rule descriptor has an invalid option name: "
                + descriptor.type;
            return false;
        }
        if (!valid_descriptor_key(option.display_name_key, 128)) {
            error = "rule descriptor has an invalid option display name key: "
                + descriptor.type + "." + option.name;
            return false;
        }
        if (!valid_option_value_type(option.value_type)) {
            error = "rule descriptor has an invalid option value type: "
                + descriptor.type + "." + option.name;
            return false;
        }
        if (option.order != index) {
            error = "rule descriptor option order is not contiguous: "
                + descriptor.type;
            return false;
        }
        if (!option_names.emplace(option.name).second) {
            error = "rule descriptor has duplicate option: "
                + descriptor.type + "." + option.name;
            return false;
        }
    }
    return true;
}

} // namespace

const char* rule_option_value_type_name(RuleOptionValueType type) noexcept {
    switch (type) {
    case RuleOptionValueType::String:
        return "string";
    case RuleOptionValueType::JsonValue:
        return "json_value";
    case RuleOptionValueType::JsonPointer:
        return "json_pointer";
    }
    return "unknown";
}

bool RuleRegistry::register_factory(
    std::shared_ptr<const RuleFactory> factory,
    std::string& error) {
    error.clear();
    if (!factory) {
        error = "cannot register a null rule factory";
        return false;
    }
    const std::string type(factory->type());
    if (!is_valid_rule_type(type)) {
        error = "invalid rule factory type: " + type;
        return false;
    }
    if (!validate_descriptor(factory->descriptor(), type, error)) {
        return false;
    }
    if (factories_.count(type) != 0) {
        error = "rule factory already registered: " + type;
        return false;
    }
    factories_.emplace(type, std::move(factory));
    return true;
}

const RuleDescriptor* RuleRegistry::find_descriptor(std::string_view rule_type) const {
    const auto factory = find(rule_type);
    return factory ? &factory->descriptor() : nullptr;
}

std::shared_ptr<const RuleFactory> RuleRegistry::find(std::string_view rule_type) const {
    const auto factory = factories_.find(std::string(rule_type));
    return factory == factories_.end() ? nullptr : factory->second;
}

bool RuleRegistry::compile_pipeline(
    const std::vector<RuleDefinition>& definitions,
    const std::shared_ptr<const ProtocolHandler>& protocol,
    std::shared_ptr<const CompiledPipeline>& pipeline,
    std::string& error) const {
    error.clear();
    if (!protocol) {
        error = "rule pipeline requires a protocol handler";
        return false;
    }
    if (definitions.size() > kMaxRulesPerProfile) {
        error = "rule pipeline exceeds the maximum rule count";
        return false;
    }

    std::unordered_set<std::string> ids;
    std::vector<std::shared_ptr<const CompiledRule>> compiled;
    compiled.reserve(definitions.size());
    for (const auto& definition : definitions) {
        if (!is_valid_rule_id(definition.id.value)) {
            error = "invalid rule id: " + definition.id.value;
            return false;
        }
        if (!is_valid_rule_type(definition.type)) {
            error = "rule " + definition.id.value + " has an invalid type";
            return false;
        }
        if (!ids.emplace(definition.id.value).second) {
            error = "duplicate rule id: " + definition.id.value;
            return false;
        }
        if (!definition.enabled) {
            continue;
        }
        const auto factory = find(definition.type);
        if (!factory) {
            error = "enabled rule " + definition.id.value
                + " has unknown type: " + definition.type;
            return false;
        }
        std::shared_ptr<const CompiledRule> rule;
        if (!factory->compile(definition, *protocol, rule, error)) {
            error = "rule " + definition.id.value + " (" + definition.type + "): " + error;
            return false;
        }
        if (!rule
            || rule->id() != definition.id.value
            || rule->type() != definition.type) {
            error = "rule factory returned an invalid compiled rule for: " + definition.id.value;
            return false;
        }
        compiled.push_back(std::move(rule));
    }

    pipeline = std::make_shared<const CompiledPipeline>(std::move(compiled));
    return true;
}

std::vector<std::string> RuleRegistry::rule_types() const {
    std::vector<std::string> types;
    types.reserve(factories_.size());
    for (const auto& [type, factory] : factories_) {
        (void)factory;
        types.push_back(type);
    }
    std::sort(types.begin(), types.end());
    return types;
}

std::vector<RuleDescriptor> RuleRegistry::descriptors() const {
    std::vector<RuleDescriptor> values;
    values.reserve(factories_.size());
    for (const auto& [type, factory] : factories_) {
        (void)type;
        values.push_back(factory->descriptor());
    }
    std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) {
        return left.type < right.type;
    });
    return values;
}

std::shared_ptr<const RuleRegistry> builtin_rule_registry() {
    static const auto registry = []() {
        auto value = std::make_shared<RuleRegistry>();
        std::string error;
        if (!value->register_factory(make_set_field_rule_factory(), error)
            || !value->register_factory(make_remove_field_rule_factory(), error)
            || !value->register_factory(make_remove_tool_rule_factory(), error)) {
            throw std::logic_error("failed to build rule registry: " + error);
        }
        return std::shared_ptr<const RuleRegistry>(std::move(value));
    }();
    return registry;
}

} // namespace ccs
