#include "app/app_service.hpp"

#include "server/server.hpp"

#include <exception>
#include <utility>

namespace ccs {

AppService::AppService(RuntimeSnapshotPtr snapshot, bool handle_process_signals)
    : snapshot_(std::move(snapshot))
    , handle_process_signals_(handle_process_signals) {}

AppService::~AppService() {
    stop();
    wait();
}

bool AppService::start(std::string& error) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (wait_in_progress_) {
        error = "service wait is already in progress";
        return false;
    }
    return start_impl(error);
}

bool AppService::start_impl(std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != ServiceState::Stopped || thread_.joinable()) {
        error = "service is already running";
        return false;
    }

    state_ = ServiceState::Starting;
    startup_complete_ = false;
    startup_succeeded_ = false;
    thread_complete_ = false;
    startup_error_.clear();
    exit_code_ = 1;
    try {
        server_ = std::make_shared<Server>(snapshot_, Server::LogSinkFactory{}, handle_process_signals_);
        const auto running_server = server_;
        thread_ = std::thread([this, running_server]() {
            const int code = running_server->run([this](bool succeeded, const std::string& startup_error) {
                {
                    std::lock_guard<std::mutex> state_lock(mutex_);
                    startup_complete_ = true;
                    startup_succeeded_ = succeeded && state_ == ServiceState::Starting;
                    startup_error_ = startup_succeeded_
                        ? std::string{}
                        : startup_error.empty() ? "service startup was cancelled" : startup_error;
                    state_ = startup_succeeded_ ? ServiceState::Running : ServiceState::Stopped;
                }
                state_cv_.notify_all();
            });

            {
                std::lock_guard<std::mutex> state_lock(mutex_);
                if (!startup_complete_) {
                    startup_complete_ = true;
                    startup_succeeded_ = false;
                    startup_error_ = "service stopped before startup completed";
                }
                exit_code_ = code;
                state_ = ServiceState::Stopped;
                thread_complete_ = true;
            }
            state_cv_.notify_all();
        });
    } catch (const std::exception& ex) {
        server_.reset();
        state_ = ServiceState::Stopped;
        thread_complete_ = true;
        error = ex.what();
        return false;
    }

    state_cv_.wait(lock, [this]() { return startup_complete_; });
    if (startup_succeeded_) {
        return true;
    }

    error = startup_error_;
    lock.unlock();
    if (thread_.joinable()) {
        thread_.join();
    }
    lock.lock();
    server_.reset();
    return false;
}

bool AppService::reload(RuntimeSnapshotPtr snapshot, std::string& error) {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (wait_in_progress_) {
        error = "service cannot reload while wait is in progress";
        return false;
    }
    if (!snapshot) {
        error = "runtime snapshot must not be null";
        return false;
    }

    std::shared_ptr<Server> running_server;
    RuntimeSnapshotPtr previous_snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ServiceState::Running || !server_) {
            error = "service must be running to reload";
            return false;
        }
        running_server = server_;
        previous_snapshot = snapshot_;
    }

    const auto result = running_server->reload(snapshot, error);
    if (result == ReloadResult::Applied) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server_ != running_server || state_ != ServiceState::Running) {
            error = "service state changed while applying reload";
            return false;
        }
        snapshot_ = std::move(snapshot);
        return true;
    }
    if (result == ReloadResult::Failed) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (server_ != running_server || state_ != ServiceState::Running) {
            error = "service state changed before restart reload";
            return false;
        }
        state_ = ServiceState::Reloading;
        running_server->request_stop();
    }
    if (thread_.joinable()) {
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_.reset();
        state_ = ServiceState::Stopped;
        snapshot_ = snapshot;
    }
    running_server.reset();

    std::string restart_error;
    if (start_impl(restart_error)) {
        return true;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot_ = previous_snapshot;
    }
    std::string rollback_error;
    if (start_impl(rollback_error)) {
        error = "reload failed and the previous configuration was restored: " + restart_error;
        return false;
    }
    error = "reload failed: " + restart_error + "; rollback failed: " + rollback_error;
    return false;
}

void AppService::stop() {
    std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
    std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == ServiceState::Stopped) {
        return;
    }
    state_ = ServiceState::Stopping;
    if (server_) {
        server_->request_stop();
    }
}

int AppService::wait() {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_);
    if (wait_in_progress_) {
        lifecycle_cv_.wait(lifecycle_lock, [this]() { return !wait_in_progress_; });
        std::lock_guard<std::mutex> lock(mutex_);
        return exit_code_;
    }
    wait_in_progress_ = true;
    lifecycle_lock.unlock();
    if (thread_.joinable()) {
        thread_.join();
    }
    int result = 1;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_.reset();
        result = exit_code_;
    }
    lifecycle_lock.lock();
    wait_in_progress_ = false;
    lifecycle_lock.unlock();
    lifecycle_cv_.notify_all();
    return result;
}

bool AppService::try_wait(int& exit_code) {
    std::unique_lock<std::mutex> lifecycle_lock(lifecycle_mutex_, std::try_to_lock);
    if (!lifecycle_lock.owns_lock() || wait_in_progress_) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (state_ != ServiceState::Stopped || !thread_complete_) {
            return false;
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        server_.reset();
        exit_code = exit_code_;
    }
    return true;
}

ServiceState AppService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

} // namespace ccs
