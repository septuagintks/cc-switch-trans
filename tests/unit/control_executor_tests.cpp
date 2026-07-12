#include "hosts/control_executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
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

} // namespace

int main() {
    try {
        test_serial_execution_and_drain();
        test_task_exception_does_not_stop_worker();
        std::cout << "control executor tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "control executor tests failed: " << ex.what() << "\n";
        return 1;
    }
}
