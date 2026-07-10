#include "core/task.hpp"
#include "transforms/findcg_responses_transform.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <sstream>
#include <string>
#include <vector>

namespace {

long long percentile(std::vector<long long> values, double fraction) {
    std::sort(values.begin(), values.end());
    const auto index = static_cast<std::size_t>((values.size() - 1) * fraction);
    return values[index];
}

std::string make_body(std::size_t target_size) {
    const std::string prefix = R"({"input":")";
    const std::string suffix = R"(","tools":[{"type":"namespace","name":"image_gen"},{"type":"function","name":"web_search"}]})";
    const auto padding_size = target_size > prefix.size() + suffix.size()
        ? target_size - prefix.size() - suffix.size()
        : 0;
    return prefix + std::string(padding_size, 'x') + suffix;
}

} // namespace

int main(int argc, char** argv) {
    const int iterations = argc > 1 ? std::max(1, std::atoi(argv[1])) : 200;
    const std::size_t body_size = argc > 2
        ? std::max<std::size_t>(1024, static_cast<std::size_t>(std::strtoull(argv[2], nullptr, 10)))
        : 100 * 1024;

    ccs::TaskConfig task{
        ccs::ApiTaskKind::Responses,
        true,
        "POST",
        "/v1/responses/",
        {"https://www.findcg.com", "/v1/responses/"},
        {"remove_findcg_image_gen"},
        true,
    };
    ccs::FindcgResponsesTransform transform;
    const auto body = make_body(body_size);

    for (int i = 0; i < 5; ++i) {
        (void)transform.apply(task, body);
    }

    std::vector<long long> durations_us;
    durations_us.reserve(static_cast<std::size_t>(iterations));
    const auto benchmark_start = std::chrono::steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        const auto started = std::chrono::steady_clock::now();
        const auto result = transform.apply(task, body);
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - started);
        if (!result.modified || result.removed_tools.size() != 1 || !result.rewritten_body) {
            std::cerr << "unexpected transform result\n";
            return 1;
        }
        durations_us.push_back(elapsed.count());
    }
    const auto total = std::chrono::duration<double>(std::chrono::steady_clock::now() - benchmark_start).count();
    const auto mean = std::accumulate(durations_us.begin(), durations_us.end(), 0.0)
        / static_cast<double>(durations_us.size());

    std::cout << std::fixed << std::setprecision(3)
              << "{\"schema_version\":\"ccs-trans-transform-benchmark/v1\""
              << ",\"iterations\":" << iterations
              << ",\"body_size\":" << body.size()
              << ",\"mean_us\":" << mean
              << ",\"p50_us\":" << percentile(durations_us, 0.50)
              << ",\"p95_us\":" << percentile(durations_us, 0.95)
              << ",\"p99_us\":" << percentile(durations_us, 0.99)
              << ",\"throughput_per_second\":" << (static_cast<double>(iterations) / total)
              << "}\n";
    return 0;
}
