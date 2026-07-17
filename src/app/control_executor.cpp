#include "app/control_executor.hpp"

#include "core/runtime_metrics.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace ccs {

namespace {

std::size_t require_capacity(std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("control executor capacity must be positive");
    }
    return capacity;
}

} // namespace

ControlExecutor::ControlExecutor(
    std::size_t capacity,
    std::shared_ptr<RuntimeMetrics> metrics)
    : capacity_(require_capacity(capacity))
    , metrics_(std::move(metrics))
    , worker_([this]() { run(); }) {}

ControlExecutor::~ControlExecutor() {
    stop();
}

bool ControlExecutor::post(std::function<void()> task) {
    return post_impl({}, std::move(task));
}

bool ControlExecutor::post_coalesced(
    std::string key,
    std::function<void()> task) {
    if (key.empty()) {
        return false;
    }
    return post_impl(std::move(key), std::move(task));
}

bool ControlExecutor::post_impl(
    std::string key,
    std::function<void()> task) {
    if (!task) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) {
            ++rejected_;
            if (metrics_) {
                metrics_->control_task_rejected();
            }
            return false;
        }
        if (!key.empty() && coalescing_keys_.count(key) != 0) {
            ++coalesced_;
            if (metrics_) {
                metrics_->control_task_coalesced();
            }
            return true;
        }
        if (tasks_.size() >= capacity_) {
            ++rejected_;
            if (metrics_) {
                metrics_->control_task_rejected();
            }
            return false;
        }
        tasks_.push(Task{std::move(task), key});
        if (!key.empty()) {
            coalescing_keys_.insert(std::move(key));
        }
        peak_pending_ = std::max(peak_pending_, tasks_.size());
        if (metrics_) {
            metrics_->control_task_queued(tasks_.size() + (running_task_ ? 1 : 0));
        }
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

ControlExecutorStatus ControlExecutor::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ControlExecutorStatus{
        capacity_,
        tasks_.size(),
        peak_pending_,
        rejected_,
        coalesced_,
        stopping_,
    };
}

void ControlExecutor::run() {
    while (true) {
        Task task;
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
            running_task_ = true;
            if (metrics_) {
                metrics_->control_task_queued(tasks_.size() + 1);
            }
        }
        try {
            task.callback();
        } catch (...) {
            // Control commands report their own failures. Keep the executor alive.
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!task.coalescing_key.empty()) {
                coalescing_keys_.erase(task.coalescing_key);
            }
            running_task_ = false;
            if (metrics_) {
                metrics_->control_task_finished(tasks_.size());
            }
        }
    }
}

} // namespace ccs
