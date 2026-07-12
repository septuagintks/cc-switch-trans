#include "hosts/windows/instance_coordinator.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_single_instance_mutex() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto name = L"Local\\ccs-trans-instance-test-" + std::to_wstring(nonce);
    std::string error;
    ccs::InstanceCoordinator first(name);
    ccs::InstanceCoordinator second(name);
    require(first.acquire(error) == ccs::InstanceAcquireResult::Acquired, error);
    require(second.acquire(error) == ccs::InstanceAcquireResult::AlreadyRunning,
        "second coordinator unexpectedly acquired the mutex");
    require(ccs::tray_show_message() != 0, "registered tray message is unavailable");
}

} // namespace

int main() {
    try {
        test_single_instance_mutex();
        std::cout << "instance coordinator tests ok\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "instance coordinator tests failed: " << ex.what() << "\n";
        return 1;
    }
}
