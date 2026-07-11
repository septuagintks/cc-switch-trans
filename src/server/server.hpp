#pragma once

#include "core/cancellation.hpp"
#include "core/http_types.hpp"
#include "core/runtime_metrics.hpp"
#include "logging/logger.hpp"
#include "routing/route_table.hpp"
#include "runtime/runtime_snapshot.hpp"
#include "transport/proxy.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>

namespace ccs {

enum class ReloadResult {
    Applied,
    RestartRequired,
    Failed,
};

class Server {
public:
    using StartupCallback = std::function<void(bool, const std::string&)>;

    explicit Server(RuntimeSnapshotPtr snapshot);

    int run(const StartupCallback& startup_callback = {});
    void request_stop();
    ReloadResult reload(RuntimeSnapshotPtr snapshot, std::string& error);
    std::string process_raw_request(
        const std::string& raw,
        const std::string& client_ip) const;
    bool process_raw_request_to_sender(
        const std::string& raw,
        const std::string& client_ip,
        const std::function<bool(const std::string&)>& sender,
        const CancellationToken& cancellation = {}) const;
    void log_request_error(
        int status_code,
        const std::string& type,
        const std::string& message) const;

private:
    class RequestGeneration;

    std::shared_ptr<RequestGeneration> current_generation() const;
    bool process_with_generation(
        const std::shared_ptr<RequestGeneration>& generation,
        const std::string& raw,
        const std::string& client_ip,
        const std::function<bool(const std::string&)>& sender,
        const CancellationToken& cancellation) const;
    HttpResponse handle_local_route_error(const RouteLookup& route) const;
    HttpResponse handle_usage_request(
        const std::shared_ptr<RequestGeneration>& generation,
        const HttpRequest& request,
        const RouteEntry& route,
        const CancellationToken& cancellation) const;
    void log_performance_snapshot(const std::string& reason) const;

    std::shared_ptr<RuntimeMetrics> metrics_;
    mutable std::shared_ptr<RequestGeneration> generation_;
    mutable std::shared_mutex generation_mutex_;
    mutable std::mutex reload_mutex_;
    std::atomic_bool stop_requested_{false};
};

} // namespace ccs
