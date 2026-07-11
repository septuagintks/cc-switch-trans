#include "protocols/protocol_registry.hpp"
#include "rules/rule_registry.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

enum class BenchmarkMode {
    Empty,
    MatchedModified,
    MatchedUnchanged,
    Unmatched,
};

const char* mode_name(BenchmarkMode mode) {
    switch (mode) {
    case BenchmarkMode::Empty:
        return "empty";
    case BenchmarkMode::MatchedModified:
        return "matched_modified";
    case BenchmarkMode::MatchedUnchanged:
        return "matched_unchanged";
    case BenchmarkMode::Unmatched:
        return "unmatched";
    }
    return "unknown";
}

ccs::RuleDefinition make_rule(
    std::size_t index,
    BenchmarkMode mode) {
    ccs::RuleDefinition definition;
    definition.id.value = "rule-" + std::to_string(index);
    definition.enabled = true;
    if (mode == BenchmarkMode::MatchedUnchanged) {
        definition.type = "set_field";
        definition.options["path"] = "/markers/" + std::to_string(index);
        definition.options["value"] = index;
    } else {
        definition.type = "remove_tool";
        definition.options["tool"] = mode == BenchmarkMode::MatchedModified
            ? "target-" + std::to_string(index)
            : "missing-" + std::to_string(index);
    }
    return definition;
}

std::shared_ptr<const ccs::CompiledPipeline> make_pipeline(
    std::size_t rule_count,
    BenchmarkMode mode) {
    std::vector<ccs::RuleDefinition> definitions;
    definitions.reserve(rule_count);
    for (std::size_t index = 0; index < rule_count; ++index) {
        definitions.push_back(make_rule(index, mode));
    }
    std::shared_ptr<const ccs::CompiledPipeline> pipeline;
    std::string error;
    if (!ccs::builtin_rule_registry()->compile_pipeline(
            definitions,
            ccs::builtin_protocol_registry()->find("responses"),
            pipeline,
            error)) {
        throw std::runtime_error(error);
    }
    return pipeline;
}

std::string make_body(
    std::size_t rule_count,
    BenchmarkMode mode,
    std::size_t target_size) {
    nlohmann::json body = {
        {"input", "benchmark"},
        {"markers", nlohmann::json::array()},
        {"payload", ""},
        {"tools", nlohmann::json::array()},
    };
    for (std::size_t index = 0; index < rule_count; ++index) {
        body["markers"].push_back(index);
        if (mode == BenchmarkMode::MatchedModified) {
            body["tools"].push_back({{"name", "target-" + std::to_string(index)}});
        }
    }
    body["tools"].push_back({{"name", "web_search"}});
    const auto base = body.dump();
    if (target_size > base.size()) {
        body["payload"] = std::string(target_size - base.size(), 'x');
    }
    return body.dump();
}

std::uint64_t percentile(std::vector<std::uint64_t> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>(
        static_cast<double>(values.size() - 1) * fraction);
    return values[index];
}

double mean(const std::vector<std::uint64_t>& values) {
    return static_cast<double>(std::accumulate(
               values.begin(),
               values.end(),
               std::uint64_t{0}))
        / static_cast<double>(values.size());
}

void run_case(
    int iterations,
    std::size_t body_size,
    std::size_t rule_count,
    BenchmarkMode mode) {
    const auto pipeline = make_pipeline(rule_count, mode);
    const auto body = make_body(rule_count, mode, body_size);
    for (int index = 0; index < 5; ++index) {
        (void)pipeline->apply(body);
    }

    std::vector<std::uint64_t> total_us;
    std::vector<std::uint64_t> parse_us;
    std::vector<std::uint64_t> rules_us;
    std::vector<std::uint64_t> serialize_us;
    total_us.reserve(static_cast<std::size_t>(iterations));
    parse_us.reserve(static_cast<std::size_t>(iterations));
    rules_us.reserve(static_cast<std::size_t>(iterations));
    serialize_us.reserve(static_cast<std::size_t>(iterations));
    for (int index = 0; index < iterations; ++index) {
        const auto started = std::chrono::steady_clock::now();
        const auto result = pipeline->apply(body);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        const bool should_modify = mode == BenchmarkMode::MatchedModified;
        if (!result.ok
            || result.modified != should_modify
            || result.parse_count != (rule_count == 0 ? 0U : 1U)
            || result.serialize_count != (should_modify ? 1U : 0U)) {
            throw std::runtime_error("unexpected benchmark pipeline result");
        }
        total_us.push_back(static_cast<std::uint64_t>(elapsed.count()));
        parse_us.push_back(result.parse_duration_us);
        rules_us.push_back(result.rules_duration_us);
        serialize_us.push_back(result.serialize_duration_us);
    }

    const nlohmann::json record = {
        {"schema_version", "ccs-trans-rule-pipeline-benchmark/v1"},
        {"mode", mode_name(mode)},
        {"rule_count", rule_count},
        {"body_size", body.size()},
        {"iterations", iterations},
        {"total_mean_us", mean(total_us)},
        {"total_p50_us", percentile(total_us, 0.50)},
        {"total_p95_us", percentile(total_us, 0.95)},
        {"parse_mean_us", mean(parse_us)},
        {"rules_mean_us", mean(rules_us)},
        {"serialize_mean_us", mean(serialize_us)},
    };
    std::cout << record.dump() << '\n';
}

} // namespace

int main(int argc, char** argv) {
    try {
        const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 100;
        std::vector<std::size_t> body_sizes = {1024, 100 * 1024, 1024 * 1024};
        if (argc > 2) {
            body_sizes = {std::max<std::size_t>(
                256,
                static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)))};
        }
        for (const auto body_size : body_sizes) {
            run_case(iterations, body_size, 0, BenchmarkMode::Empty);
            for (const auto count : {1U, 8U, 32U}) {
                run_case(iterations, body_size, count, BenchmarkMode::MatchedModified);
                run_case(iterations, body_size, count, BenchmarkMode::MatchedUnchanged);
                run_case(iterations, body_size, count, BenchmarkMode::Unmatched);
            }
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "rule pipeline benchmark failed: " << ex.what() << '\n';
        return 1;
    }
}
