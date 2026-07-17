#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_set>

namespace ccs {

class RuntimeMetrics;

inline constexpr std::size_t kDefaultControlQueueCapacity = 64;

struct ControlExecutorStatus {
    std::size_t capacity = 0;
    std::size_t pending = 0;
    std::size_t peak_pending = 0;
    std::size_t rejected = 0;
    std::size_t coalesced = 0;
    bool stopping = false;
};

class ControlExecutor {
public:
    explicit ControlExecutor(
        std::size_t capacity = kDefaultControlQueueCapacity,
        std::shared_ptr<RuntimeMetrics> metrics = {});
    ~ControlExecutor();

    ControlExecutor(const ControlExecutor&) = delete;
    ControlExecutor& operator=(const ControlExecutor&) = delete;

    bool post(std::function<void()> task);
    bool post_coalesced(std::string key, std::function<void()> task);
    void stop();
    std::size_t pending() const;
    ControlExecutorStatus status() const;

private:
    struct Task {
        std::function<void()> callback;
        std::string coalescing_key;
    };

    bool post_impl(std::string key, std::function<void()> task);
    void run();

    const std::size_t capacity_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::queue<Task> tasks_;
    std::unordered_set<std::string> coalescing_keys_;
    std::size_t peak_pending_ = 0;
    std::size_t rejected_ = 0;
    std::size_t coalesced_ = 0;
    bool running_task_ = false;
    bool stopping_ = false;
    std::thread worker_;
};

} // namespace ccs
