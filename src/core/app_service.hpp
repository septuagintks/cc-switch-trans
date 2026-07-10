#pragma once

#include "config/config.hpp"

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
    Stopping,
};

class AppService {
public:
    explicit AppService(AppConfig config);
    ~AppService();

    AppService(const AppService&) = delete;
    AppService& operator=(const AppService&) = delete;

    bool start(std::string& error);
    void stop();
    int wait();
    ServiceState status() const;

private:
    AppConfig config_;
    mutable std::mutex mutex_;
    std::condition_variable state_cv_;
    ServiceState state_ = ServiceState::Stopped;
    int exit_code_ = 1;
    std::unique_ptr<Server> server_;
    std::thread thread_;
};

} // namespace ccs
