#include "rules/rule_registry.hpp"

#include "rules/generic_json_rules.hpp"
#include "rules/remove_tool_rule.hpp"

#include <algorithm>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace ccs {

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
    if (factories_.count(type) != 0) {
        error = "rule factory already registered: " + type;
        return false;
    }
    factories_.emplace(type, std::move(factory));
    return true;
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
