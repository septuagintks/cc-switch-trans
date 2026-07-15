#pragma once

#include "app/application_control.hpp"
#include "app/application_status.hpp"
#include "config/app_paths.hpp"
#include "runtime/runtime_snapshot.hpp"

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

class ApplicationController final : public ApplicationControl {
public:
    explicit ApplicationController(AppPaths paths);
    ~ApplicationController();

    ApplicationController(const ApplicationController&) = delete;
    ApplicationController& operator=(const ApplicationController&) = delete;

    bool start(std::string& error) override;
    bool reload(std::string& error) override;
    bool stop(std::string& error) override;
    bool shutdown(std::string& error) override;
    ApplicationStatus status() override;

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
