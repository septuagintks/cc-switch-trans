#pragma once

#include "config/config_document.hpp"
#include "protocols/protocol_handler.hpp"
#include "rules/rule.hpp"

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace ccs {

class RuleFactory {
public:
    virtual ~RuleFactory() = default;

    virtual std::string_view type() const noexcept = 0;
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
    bool compile_pipeline(
        const std::vector<RuleDefinition>& definitions,
        const std::shared_ptr<const ProtocolHandler>& protocol,
        std::shared_ptr<const CompiledPipeline>& pipeline,
        std::string& error) const;
    std::vector<std::string> rule_types() const;

private:
    std::unordered_map<std::string, std::shared_ptr<const RuleFactory>> factories_;
};

std::shared_ptr<const RuleRegistry> builtin_rule_registry();

} // namespace ccs
