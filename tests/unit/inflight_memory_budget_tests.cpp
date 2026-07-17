#include "core/inflight_memory_budget.hpp"
#include "core/runtime_metrics.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_lease_lifecycle() {
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    ccs::InflightMemoryBudget budget(10, metrics);
    require(budget.capacity() == 10, "budget exposes its configured capacity");

    auto zero = budget.try_acquire(0);
    require(zero && zero->valid() && zero->bytes() == 0,
        "zero-byte acquisition returns a valid no-op lease");

    auto first = budget.try_acquire(6);
    require(first && first->bytes() == 6,
        "first lease reserves its requested bytes");
    require(!budget.try_acquire(5), "acquisition over capacity is rejected");
    auto second = budget.try_acquire(4);
    require(second && second->bytes() == 4,
        "remaining capacity can be acquired exactly");

    first->shrink(2);
    require(first->bytes() == 4 && first->try_grow(2) && first->bytes() == 6,
        "lease shrink and grow update owned bytes");
    require(!first->try_grow(1), "lease growth respects total capacity");

    ccs::InflightMemoryBudget::Lease moved = std::move(*first);
    require(moved.valid() && moved.bytes() == 6 && !first->valid(),
        "moving a lease transfers accounting ownership");
    moved.reset();
    second.reset();
    zero.reset();

    const auto snapshot = metrics->snapshot();
    require(snapshot.inflight_budget_bytes == 10
            && snapshot.current_inflight_bytes == 0
            && snapshot.peak_inflight_bytes == 10
            && snapshot.inflight_budget_rejections == 2,
        "budget metrics retain capacity, peak, rejection, and final drain");
}

void test_lease_outlives_budget_facade() {
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    std::optional<ccs::InflightMemoryBudget::Lease> lease;
    {
        ccs::InflightMemoryBudget budget(32, metrics);
        lease = budget.try_acquire(12);
    }
    require(lease && lease->valid()
            && metrics->snapshot().current_inflight_bytes == 12,
        "lease keeps accounting alive after the budget facade is destroyed");
    lease.reset();
    require(metrics->snapshot().current_inflight_bytes == 0,
        "outliving lease releases its bytes exactly once");

    bool mismatch_rejected = false;
    try {
        ccs::InflightMemoryBudget mismatch(64, metrics);
        (void)mismatch;
    } catch (const std::invalid_argument&) {
        mismatch_rejected = true;
    }
    require(mismatch_rejected,
        "one metrics instance cannot silently change budget capacity");
}

void test_concurrent_accounting() {
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    ccs::InflightMemoryBudget budget(64, metrics);
    std::atomic_bool failed{false};
    std::vector<std::thread> workers;
    for (int worker = 0; worker < 8; ++worker) {
        workers.emplace_back([&budget, &failed]() {
            for (int iteration = 0; iteration < 5000; ++iteration) {
                auto lease = budget.try_acquire(1);
                if (!lease) {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    const auto snapshot = metrics->snapshot();
    require(!failed.load(std::memory_order_relaxed)
            && snapshot.current_inflight_bytes == 0
            && snapshot.peak_inflight_bytes > 0
            && snapshot.peak_inflight_bytes <= 64
            && snapshot.inflight_budget_rejections == 0,
        "concurrent leases remain bounded and drain to zero");
}

void test_generation_metrics() {
    ccs::RuntimeMetrics metrics;
    metrics.generation_request_started();
    metrics.generation_request_started();
    metrics.generation_retired();
    metrics.generation_retired();
    metrics.generation_request_finished();
    metrics.retired_generation_released();
    metrics.generation_request_finished();
    metrics.retired_generation_released();

    const auto snapshot = metrics.snapshot();
    require(snapshot.current_generation_requests == 0
            && snapshot.peak_generation_requests == 2
            && snapshot.current_retired_generations == 0
            && snapshot.peak_retired_generations == 2,
        "generation request and retirement metrics retain peaks and drain");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"lease lifecycle", test_lease_lifecycle},
        {"lease outlives facade", test_lease_outlives_budget_facade},
        {"concurrent accounting", test_concurrent_accounting},
        {"generation metrics", test_generation_metrics},
    };
    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& ex) {
            std::cerr << name << " failed: " << ex.what() << "\n";
            return 1;
        }
    }
    std::cout << "inflight memory budget tests ok\n";
    return 0;
}
