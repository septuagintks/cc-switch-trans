#pragma once

#include "rules/rule_json.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace ccs {

struct RuleChangeSummary {
    std::string target;
    std::size_t affected_count = 0;
};

struct RuleApplyResult {
    bool matched = false;
    bool modified = false;
    std::string reason;
    RuleChangeSummary summary;
};

struct RuleTrace {
    std::string rule_id;
    std::string rule_type;
    bool matched = false;
    bool modified = false;
    std::string reason;
    RuleChangeSummary summary;
    std::uint64_t duration_us = 0;
};

struct RulePipelineError {
    int status_code = 400;
    std::string type = "invalid_request_error";
    std::string message;
    std::string rule_id;
    std::string rule_type;
    std::string reason;
};

struct RulePipelineResult {
    bool ok = true;
    bool modified = false;
    std::size_t parse_count = 0;
    std::size_t serialize_count = 0;
    std::uint64_t parse_duration_us = 0;
    std::uint64_t rules_duration_us = 0;
    std::uint64_t serialize_duration_us = 0;
    std::size_t original_body_size = 0;
    std::size_t output_body_size = 0;
    std::shared_ptr<InflightMemoryBudget::Lease> rewritten_body_memory;
    std::optional<std::string> rewritten_body;
    std::vector<RuleTrace> traces;
    std::optional<RulePipelineError> error;
};

class RuleRuntimeError : public std::runtime_error {
public:
    RuleRuntimeError(std::string reason, std::string message);

    const std::string& reason() const;

private:
    std::string reason_;
};

class CompiledRule {
public:
    CompiledRule(std::string id, std::string type);
    virtual ~CompiledRule() = default;

    const std::string& id() const;
    const std::string& type() const;
    virtual RuleApplyResult apply(RuleJson& body) const = 0;

private:
    std::string id_;
    std::string type_;
};

class CompiledPipeline {
public:
    explicit CompiledPipeline(std::vector<std::shared_ptr<const CompiledRule>> rules = {});

    RulePipelineResult apply(
        const std::string& body,
        std::shared_ptr<InflightMemoryBudget> inflight_budget = {}) const;
    std::size_t size() const;
    bool empty() const;

private:
    std::vector<std::shared_ptr<const CompiledRule>> rules_;
};

} // namespace ccs
