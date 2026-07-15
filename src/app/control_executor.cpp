#include "app/control_executor.hpp"

#include <utility>

namespace ccs {

ControlExecutor::ControlExecutor()
    : worker_([this]() { run(); }) {}

ControlExecutor::~ControlExecutor() {
    stop();
}

bool ControlExecutor::post(std::function<void()> task) {
    if (!task) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            return false;
        }
        tasks_.push(std::move(task));
    }
    cv_.notify_one();
    return true;
}

void ControlExecutor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

std::size_t ControlExecutor::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void ControlExecutor::run() {
    while (true) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                if (stopping_) {
                    return;
                }
                continue;
            }
            task = std::move(tasks_.front());
            tasks_.pop();
        }
        try {
            task();
        } catch (...) {
            // Control commands report their own failures. Keep the executor alive.
        }
    }
}

} // namespace ccs
