#pragma once

#include "config/app_paths.hpp"
#include "runtime/runtime_snapshot.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace ccs {

class AppService;

struct RuntimeLoadOptions {
    std::string selected_profile;
    std::string log_level;
    std::string log_path;
};

bool load_runtime_snapshot(
    const AppPaths& paths,
    const RuntimeLoadOptions& options,
    RuntimeSnapshotPtr& snapshot,
    std::string& error);

enum class ApplicationState {
    Stopped,
    Starting,
    Running,
    Reloading,
    Stopping,
    Faulted,
    Shutdown,
};

struct ApplicationStatus {
    ApplicationState state = ApplicationState::Stopped;
    std::string listener_host;
    std::uint16_t listener_port = 0;
    std::string last_error;
    int last_exit_code = 0;
};

const char* application_state_name(ApplicationState state);

class ApplicationController {
public:
    explicit ApplicationController(AppPaths paths);
    ~ApplicationController();

    ApplicationController(const ApplicationController&) = delete;
    ApplicationController& operator=(const ApplicationController&) = delete;

    bool start(std::string& error);
    bool reload(std::string& error);
    bool stop(std::string& error);
    bool shutdown(std::string& error);
    ApplicationStatus status();

    const AppPaths& paths() const;

private:
    bool stop_impl(std::string& error);
    void refresh_completed_service();
    void update_status(
        ApplicationState state,
        const std::string& error = {},
        int exit_code = 0);
    ApplicationStatus status_snapshot() const;

    AppPaths paths_;
    std::mutex command_mutex_;
    mutable std::mutex status_mutex_;
    ApplicationStatus status_;
    RuntimeSnapshotPtr snapshot_;
    std::unique_ptr<AppService> service_;
    bool shutdown_ = false;
};

} // namespace ccs
