#pragma once

#include "config/config_document.hpp"
#include "protocols/protocol_handler.hpp"
#include "rules/rule.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ccs {

enum class RuleOptionValueType {
    String,
    JsonValue,
    JsonPointer,
};

struct RuleOptionDescriptor {
    std::string name;
    std::string display_name_key;
    RuleOptionValueType value_type = RuleOptionValueType::String;
    bool required = false;
    std::size_t order = 0;

    bool operator==(const RuleOptionDescriptor&) const = default;
};

struct RuleDescriptor {
    std::string type;
    std::string display_name_key;
    bool protocol_specialized = false;
    std::vector<RuleOptionDescriptor> options;

    bool operator==(const RuleDescriptor&) const = default;
};

const char* rule_option_value_type_name(RuleOptionValueType type) noexcept;

class RuleFactory {
public:
    virtual ~RuleFactory() = default;

    virtual std::string_view type() const noexcept = 0;
    virtual const RuleDescriptor& descriptor() const noexcept = 0;
    virtual bool compile(
        const RuleDefinition& definition,
        const ProtocolHandler& protocol,
        std::shared_ptr<const CompiledRule>& rule,
        std::string& error) const = 0;
};

class RuleRegistry {
public:
    bool register_factory(
        std::shared_ptr<const RuleFactory> factory,
        std::string& error);

    std::shared_ptr<const RuleFactory> find(std::string_view rule_type) const;
    const RuleDescriptor* find_descriptor(std::string_view rule_type) const;
    bool compile_pipeline(
        const std::vector<RuleDefinition>& definitions,
        const std::shared_ptr<const ProtocolHandler>& protocol,
        std::shared_ptr<const CompiledPipeline>& pipeline,
        std::string& error) const;
    std::vector<std::string> rule_types() const;
    std::vector<RuleDescriptor> descriptors() const;

private:
    std::unordered_map<std::string, std::shared_ptr<const RuleFactory>> factories_;
};

std::shared_ptr<const RuleRegistry> builtin_rule_registry();

} // namespace ccs
