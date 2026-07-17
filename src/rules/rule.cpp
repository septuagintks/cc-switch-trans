#include "rules/rule.hpp"

#include <chrono>
#include <utility>

namespace ccs {

namespace {

RuleTrace make_trace(
    const CompiledRule& rule,
    const RuleApplyResult& result,
    std::uint64_t duration_us) {
    return RuleTrace{
        rule.id(),
        rule.type(),
        result.matched,
        result.modified,
        result.reason,
        result.summary,
        duration_us,
    };
}

std::uint64_t elapsed_us(std::chrono::steady_clock::time_point started) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started)
            .count());
}

} // namespace

RuleRuntimeError::RuleRuntimeError(std::string reason, std::string message)
    : std::runtime_error(std::move(message))
    , reason_(std::move(reason)) {}

const std::string& RuleRuntimeError::reason() const {
    return reason_;
}

CompiledRule::CompiledRule(std::string id, std::string type)
    : id_(std::move(id))
    , type_(std::move(type)) {}

const std::string& CompiledRule::id() const {
    return id_;
}

const std::string& CompiledRule::type() const {
    return type_;
}

CompiledPipeline::CompiledPipeline(
    std::vector<std::shared_ptr<const CompiledRule>> rules)
    : rules_(std::move(rules)) {}

RulePipelineResult CompiledPipeline::apply(
    const std::string& body,
    std::shared_ptr<InflightMemoryBudget> inflight_budget) const {
    RulePipelineResult result;
    result.original_body_size = body.size();
    result.output_body_size = body.size();
    if (rules_.empty()) {
        return result;
    }

    RuleJsonAllocationContext allocation_context(inflight_budget);
    RuleJsonAllocationScope allocation_scope(&allocation_context);
    RuleJson candidate;
    const auto parse_started = std::chrono::steady_clock::now();
    result.parse_count = 1;
    try {
        candidate = RuleJson::parse(body);
    } catch (const InflightBudgetExceeded&) {
        result.parse_duration_us = elapsed_us(parse_started);
        result.ok = false;
        result.error = RulePipelineError{
            503,
            "server_error",
            "inflight memory budget exhausted",
            {},
            {},
            "inflight_budget_exhausted",
        };
        return result;
    } catch (const RuleJson::parse_error&) {
        result.parse_duration_us = elapsed_us(parse_started);
        result.ok = false;
        result.error = RulePipelineError{
            400,
            "invalid_request_error",
            "request body is not valid JSON",
            {},
            {},
            "invalid_json",
        };
        return result;
    } catch (const RuleJson::exception&) {
        result.parse_duration_us = elapsed_us(parse_started);
        result.ok = false;
        result.error = RulePipelineError{
            400,
            "invalid_request_error",
            "request body could not be parsed as JSON",
            {},
            {},
            "json_parse_failed",
        };
        return result;
    } catch (...) {
        result.parse_duration_us = elapsed_us(parse_started);
        result.ok = false;
        result.error = RulePipelineError{
            500,
            "server_error",
            "failed to parse request JSON",
            {},
            {},
            "json_parse_internal_error",
        };
        return result;
    }
    result.parse_duration_us = elapsed_us(parse_started);

    result.traces.reserve(rules_.size());
    bool candidate_modified = false;
    const auto rules_started = std::chrono::steady_clock::now();
    for (const auto& rule : rules_) {
        if (!rule) {
            result.rules_duration_us = elapsed_us(rules_started);
            result.ok = false;
            result.error = RulePipelineError{
                500,
                "server_error",
                "compiled rule pipeline contains a null rule",
                {},
                {},
                "null_compiled_rule",
            };
            return result;
        }
        const auto started = std::chrono::steady_clock::now();
        try {
            const auto rule_result = rule->apply(candidate);
            const auto duration_us = elapsed_us(started);
            candidate_modified = candidate_modified || rule_result.modified;
            result.traces.push_back(make_trace(*rule, rule_result, duration_us));
        } catch (const InflightBudgetExceeded&) {
            const auto duration_us = elapsed_us(started);
            result.rules_duration_us = elapsed_us(rules_started);
            result.ok = false;
            result.modified = false;
            result.error = RulePipelineError{
                503,
                "server_error",
                "inflight memory budget exhausted",
                rule->id(),
                rule->type(),
                "inflight_budget_exhausted",
            };
            result.traces.push_back(RuleTrace{
                rule->id(),
                rule->type(),
                false,
                false,
                "inflight_budget_exhausted",
                {},
                duration_us,
            });
            return result;
        } catch (const RuleRuntimeError& ex) {
            const auto duration_us = elapsed_us(started);
            result.rules_duration_us = elapsed_us(rules_started);
            result.ok = false;
            result.modified = false;
            result.error = RulePipelineError{
                400,
                "invalid_request_error",
                ex.what(),
                rule->id(),
                rule->type(),
                ex.reason(),
            };
            result.traces.push_back(RuleTrace{
                rule->id(),
                rule->type(),
                false,
                false,
                ex.reason(),
                {},
                duration_us,
            });
            return result;
        } catch (...) {
            const auto duration_us = elapsed_us(started);
            result.rules_duration_us = elapsed_us(rules_started);
            result.ok = false;
            result.modified = false;
            result.error = RulePipelineError{
                500,
                "server_error",
                "rule execution failed",
                rule->id(),
                rule->type(),
                "unexpected_rule_error",
            };
            result.traces.push_back(RuleTrace{
                rule->id(),
                rule->type(),
                false,
                false,
                "unexpected_rule_error",
                {},
                duration_us,
            });
            return result;
        }
    }
    result.rules_duration_us = elapsed_us(rules_started);

    if (!candidate_modified) {
        return result;
    }
    const auto serialize_started = std::chrono::steady_clock::now();
    result.serialize_count = 1;
    try {
        const auto serialized = candidate.dump();
        if (inflight_budget) {
            auto memory = inflight_budget->try_acquire(serialized.size());
            if (!memory) {
                throw InflightBudgetExceeded();
            }
            result.rewritten_body_memory =
                std::make_shared<InflightMemoryBudget::Lease>(std::move(*memory));
        }
        result.rewritten_body.emplace(serialized.data(), serialized.size());
        result.serialize_duration_us = elapsed_us(serialize_started);
        result.modified = true;
        result.output_body_size = result.rewritten_body->size();
        return result;
    } catch (const InflightBudgetExceeded&) {
        result.serialize_duration_us = elapsed_us(serialize_started);
        result.ok = false;
        result.modified = false;
        result.rewritten_body.reset();
        result.rewritten_body_memory.reset();
        result.error = RulePipelineError{
            503,
            "server_error",
            "inflight memory budget exhausted",
            {},
            {},
            "inflight_budget_exhausted",
        };
        result.output_body_size = body.size();
        return result;
    } catch (...) {
        result.serialize_duration_us = elapsed_us(serialize_started);
        result.ok = false;
        result.modified = false;
        result.rewritten_body.reset();
        result.rewritten_body_memory.reset();
        result.error = RulePipelineError{
            500,
            "server_error",
            "failed to serialize rewritten request body",
            {},
            {},
            "json_serialize_failed",
        };
        result.output_body_size = body.size();
        return result;
    }
}

std::size_t CompiledPipeline::size() const {
    return rules_.size();
}

bool CompiledPipeline::empty() const {
    return rules_.empty();
}

} // namespace ccs
