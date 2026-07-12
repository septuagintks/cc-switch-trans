#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>

namespace ccs {

class ControlExecutor {
public:
    ControlExecutor();
    ~ControlExecutor();

    ControlExecutor(const ControlExecutor&) = delete;
    ControlExecutor& operator=(const ControlExecutor&) = delete;

    bool post(std::function<void()> task);
    void stop();
    std::size_t pending() const;

private:
    void run();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<std::function<void()>> tasks_;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace ccs
