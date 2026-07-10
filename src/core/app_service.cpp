#include "core/app_service.hpp"

#include "server/server.hpp"

#include <exception>
#include <utility>

namespace ccs {

AppService::AppService(AppConfig config)
    : AppService(make_config_snapshot(std::move(config))) {}

AppService::AppService(ConfigSnapshot config)
    : config_(std::move(config)) {}

AppService::~AppService() {
    stop();
    wait();
}

bool AppService::start(std::string& error) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (state_ != ServiceState::Stopped || thread_.joinable()) {
        error = "service is already running";
        return false;
    }

    state_ = ServiceState::Starting;
    startup_complete_ = false;
    startup_succeeded_ = false;
    startup_error_.clear();
    exit_code_ = 1;
    try {
        server_ = std::make_unique<Server>(config_);
        thread_ = std::thread([this]() {
            const int code = server_->run([this](bool succeeded, const std::string& startup_error) {
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
            }
            state_cv_.notify_all();
        });
    } catch (const std::exception& ex) {
        server_.reset();
        state_ = ServiceState::Stopped;
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

void AppService::stop() {
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
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    server_.reset();
    return exit_code_;
}

ServiceState AppService::status() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

} // namespace ccs
