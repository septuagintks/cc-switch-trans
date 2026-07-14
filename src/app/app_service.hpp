#pragma once

#include "runtime/runtime_snapshot.hpp"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace ccs {

class Server;

enum class ServiceState {
    Stopped,
    Starting,
    Running,
    Reloading,
    Stopping,
};

class AppService {
public:
    explicit AppService(
        RuntimeSnapshotPtr snapshot,
        bool handle_process_signals = true);
    ~AppService();

    AppService(const AppService&) = delete;
    AppService& operator=(const AppService&) = delete;

    bool start(std::string& error);
    bool reload(RuntimeSnapshotPtr snapshot, std::string& error);
    void stop();
    int wait();
    bool try_wait(int& exit_code);
    ServiceState status() const;

private:
    bool start_impl(std::string& error);

    RuntimeSnapshotPtr snapshot_;
    bool handle_process_signals_ = true;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_cv_;
    bool wait_in_progress_ = false;
    mutable std::mutex mutex_;
    std::condition_variable state_cv_;
    ServiceState state_ = ServiceState::Stopped;
    bool startup_complete_ = false;
    bool startup_succeeded_ = false;
    bool thread_complete_ = true;
    std::string startup_error_;
    int exit_code_ = 1;
    std::shared_ptr<Server> server_;
    std::thread thread_;
};

} // namespace ccs
