#include "app/control_executor.hpp"
#include "core/runtime_metrics.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_serial_execution_and_drain() {
    ccs::ControlExecutor executor;
    std::vector<int> order;
    std::mutex order_mutex;
    std::promise<void> completed;
    auto ready = completed.get_future();

    for (int value = 0; value < 8; ++value) {
        require(executor.post([&, value]() {
            std::lock_guard<std::mutex> lock(order_mutex);
            order.push_back(value);
            if (value == 7) {
                completed.set_value();
            }
        }), "executor rejected a task while running");
    }
    require(ready.wait_for(2s) == std::future_status::ready, "executor did not run queued tasks");
    executor.stop();
    require(order == std::vector<int>({0, 1, 2, 3, 4, 5, 6, 7}),
        "executor did not preserve FIFO order");
    require(!executor.post([]() {}), "executor accepted a task after stop");
}

void test_task_exception_does_not_stop_worker() {
    ccs::ControlExecutor executor;
    std::promise<void> completed;
    auto ready = completed.get_future();
    require(executor.post([]() { throw std::runtime_error("injected"); }),
        "executor rejected throwing task");
    require(executor.post([&]() { completed.set_value(); }),
        "executor rejected task after throwing task");
    require(ready.wait_for(2s) == std::future_status::ready,
        "throwing task stopped the executor worker");
}

void test_capacity_coalescing_and_metrics() {
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    ccs::ControlExecutor executor(2, metrics);
    std::promise<void> started;
    std::promise<void> release;
    auto started_ready = started.get_future();
    auto release_ready = release.get_future().share();
    require(executor.post([&]() {
        started.set_value();
        release_ready.wait();
    }), "executor rejected blocking task");
    require(started_ready.wait_for(2s) == std::future_status::ready,
        "executor did not start blocking task");

    std::atomic_int refresh_runs{0};
    require(executor.post_coalesced("status", [&]() { ++refresh_runs; }),
        "executor rejected first coalesced task");
    require(executor.post_coalesced("status", [&]() { ++refresh_runs; }),
        "duplicate coalesced task should be treated as accepted");
    require(executor.post([]() {}), "executor rejected task at queue capacity");
    require(!executor.post([]() {}), "executor accepted task beyond queue capacity");

    const auto saturated = executor.status();
    require(saturated.capacity == 2
            && saturated.pending == 2
            && saturated.peak_pending == 2
            && saturated.coalesced == 1
            && saturated.rejected == 1,
        "executor status does not expose bounded/coalesced queue state");

    release.set_value();
    executor.stop();
    require(refresh_runs.load() == 1, "coalesced task ran more than once");
    const auto stopped = executor.status();
    const auto snapshot = metrics->snapshot();
    require(stopped.pending == 0 && stopped.stopping,
        "executor did not drain accepted tasks before stopping");
    require(snapshot.current_control_tasks == 0
            && snapshot.peak_control_tasks >= 2
            && snapshot.control_tasks_rejected == 1
            && snapshot.control_tasks_coalesced == 1,
        "control task metrics do not retain peaks and final drain");
}

void test_invalid_capacity_is_rejected() {
    bool rejected = false;
    try {
        ccs::ControlExecutor executor(0);
        (void)executor;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    require(rejected, "zero-capacity executor was accepted");
}

} // namespace

int main() {
    try {
        test_serial_execution_and_drain();
        test_task_exception_does_not_stop_worker();
        test_capacity_coalescing_and_metrics();
        test_invalid_capacity_is_rejected();
        std::cout << "control executor tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "control executor tests failed: " << ex.what() << "\n";
        return 1;
    }
}
