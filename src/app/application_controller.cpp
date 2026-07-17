#include "app/application_controller.hpp"

#include "app/app_service.hpp"
#include "config/composite_config_repository.hpp"
#include "config/runtime_compiler.hpp"

#include <utility>

namespace ccs {

bool load_runtime_snapshot(
    const AppPaths& paths,
    const RuntimeLoadOptions& options,
    RuntimeSnapshotPtr& snapshot,
    std::string& error) {
    error.clear();
    if (!ensure_app_directories(paths, error)) {
        return false;
    }

    CompositeConfigRepository repository(paths);
    if (!repository.load(error)) {
        return false;
    }
    auto configuration = repository.snapshot();
    if (!options.log_level.empty()) {
        configuration.application.logging.level = options.log_level;
    }
    if (!options.log_path.empty()) {
        configuration.application.logging.path = options.log_path;
    }

    RuntimeCompileOptions compile_options;
    if (!options.selected_profile.empty()) {
        compile_options.selected_profile = options.selected_profile;
    }
    RuntimeCompiler compiler(paths.root);
    return compiler.compile(configuration, compile_options, snapshot, error);
}

const char* application_state_name(ApplicationState state) {
    switch (state) {
    case ApplicationState::Stopped:
        return "stopped";
    case ApplicationState::Starting:
        return "starting";
    case ApplicationState::Running:
        return "running";
    case ApplicationState::Reloading:
        return "reloading";
    case ApplicationState::Stopping:
        return "stopping";
    case ApplicationState::Faulted:
        return "faulted";
    case ApplicationState::Shutdown:
        return "shutdown";
    }
    return "unknown";
}

ApplicationController::ApplicationController(AppPaths paths)
    : paths_(std::move(paths)) {}

ApplicationController::~ApplicationController() {
    std::string error;
    (void)shutdown(error);
}

bool ApplicationController::start(std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    refresh_completed_service();
    if (shutdown_) {
        error = "application controller is shut down";
        return false;
    }
    const auto current = status_snapshot();
    if (current.state != ApplicationState::Stopped
        && current.state != ApplicationState::Faulted) {
        error = "application service cannot start while "
            + std::string(application_state_name(current.state));
        return false;
    }

    update_status(ApplicationState::Starting);
    RuntimeSnapshotPtr candidate;
    if (!load_runtime_snapshot(paths_, {}, candidate, error)) {
        update_status(ApplicationState::Faulted, error, 1);
        return false;
    }

    auto candidate_service = std::make_unique<AppService>(candidate, false);
    if (!candidate_service->start(error)) {
        if (error.empty()) {
            error = "application service failed to start";
        }
        const int exit_code = candidate_service->wait();
        update_status(ApplicationState::Faulted, error, exit_code);
        return false;
    }

    snapshot_ = std::move(candidate);
    service_ = std::move(candidate_service);
    {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_.state = ApplicationState::Running;
        status_.listener_host = snapshot_->application.listener.host;
        status_.listener_port = snapshot_->application.listener.port;
        status_.last_error.clear();
        status_.last_exit_code = 0;
    }
    return true;
}

bool ApplicationController::reload(std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    refresh_completed_service();
    if (shutdown_) {
        error = "application controller is shut down";
        return false;
    }
    if (!service_ || status_snapshot().state != ApplicationState::Running) {
        error = "application service must be running to reload";
        return false;
    }

    update_status(ApplicationState::Reloading);
    RuntimeSnapshotPtr candidate;
    if (!load_runtime_snapshot(paths_, {}, candidate, error)) {
        update_status(ApplicationState::Running, error);
        return false;
    }
    if (!service_->reload(candidate, error)) {
        if (error.empty()) {
            error = "application service failed to reload";
        }
        if (service_->status() == ServiceState::Running) {
            update_status(ApplicationState::Running, error);
        } else {
            int exit_code = 1;
            (void)service_->try_wait(exit_code);
            service_.reset();
            snapshot_.reset();
            update_status(ApplicationState::Faulted, error, exit_code);
        }
        return false;
    }

    snapshot_ = std::move(candidate);
    {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        status_.state = ApplicationState::Running;
        status_.listener_host = snapshot_->application.listener.host;
        status_.listener_port = snapshot_->application.listener.port;
        status_.last_error.clear();
        status_.last_exit_code = 0;
    }
    return true;
}

bool ApplicationController::stop(std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    refresh_completed_service();
    if (shutdown_) {
        error = "application controller is shut down";
        return false;
    }
    return stop_impl(error);
}

bool ApplicationController::shutdown(std::string& error) {
    error.clear();
    std::lock_guard<std::mutex> command_lock(command_mutex_);
    if (shutdown_) {
        return true;
    }
    refresh_completed_service();
    const bool stopped = stop_impl(error);
    shutdown_ = true;
    update_status(ApplicationState::Shutdown, stopped ? std::string{} : error,
        stopped ? 0 : status_snapshot().last_exit_code);
    return stopped;
}

ApplicationStatus ApplicationController::status() {
    std::unique_lock<std::mutex> command_lock(command_mutex_, std::try_to_lock);
    if (command_lock.owns_lock()) {
        refresh_completed_service();
    }
    return status_snapshot();
}

const AppPaths& ApplicationController::paths() const {
    return paths_;
}

bool ApplicationController::stop_impl(std::string& error) {
    const auto current = status_snapshot();
    if (!service_) {
        snapshot_.reset();
        update_status(ApplicationState::Stopped);
        return true;
    }
    if (current.state == ApplicationState::Stopped) {
        service_.reset();
        snapshot_.reset();
        return true;
    }

    update_status(ApplicationState::Stopping);
    service_->stop();
    const int exit_code = service_->wait();
    service_.reset();
    snapshot_.reset();
    if (exit_code != 0) {
        error = "application service stopped with exit code " + std::to_string(exit_code);
        update_status(ApplicationState::Faulted, error, exit_code);
        return false;
    }
    update_status(ApplicationState::Stopped);
    return true;
}

void ApplicationController::refresh_completed_service() {
    if (!service_) {
        return;
    }
    int exit_code = 0;
    if (!service_->try_wait(exit_code)) {
        return;
    }

    service_.reset();
    snapshot_.reset();
    if (status_snapshot().state == ApplicationState::Stopping && exit_code == 0) {
        update_status(ApplicationState::Stopped);
        return;
    }
    const auto message = "application service exited unexpectedly with exit code "
        + std::to_string(exit_code);
    update_status(ApplicationState::Faulted, message, exit_code);
}

void ApplicationController::update_status(
    ApplicationState state,
    const std::string& error,
    int exit_code) {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    status_.state = state;
    status_.last_error = error;
    status_.last_exit_code = exit_code;
    if (state == ApplicationState::Stopped
        || state == ApplicationState::Faulted
        || state == ApplicationState::Shutdown) {
        status_.listener_host.clear();
        status_.listener_port = 0;
    }
}

ApplicationStatus ApplicationController::status_snapshot() const {
    std::lock_guard<std::mutex> status_lock(status_mutex_);
    return status_;
}

} // namespace ccs
