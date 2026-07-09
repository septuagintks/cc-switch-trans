#include "request_id.hpp"

#include <atomic>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <thread>

namespace ccs {

std::string make_request_id() {
    static std::atomic<unsigned long long> counter{0};
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);

    std::ostringstream out;
    out << "req_"
        << std::hex << millis
        << "_"
        << std::hash<std::thread::id>{}(std::this_thread::get_id())
        << "_"
        << seq;
    return out.str();
}

} // namespace ccs
