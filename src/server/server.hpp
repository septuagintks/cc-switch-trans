#pragma once

#include "core/cancellation.hpp"
#include "core/http_types.hpp"
#include "core/inflight_memory_budget.hpp"
#include "core/runtime_metrics.hpp"
#include "logging/logger.hpp"
#include "routing/route_table.hpp"
#include "runtime/runtime_snapshot.hpp"
#include "transport/upstream_transport.hpp"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>

namespace ccs {

enum class ReloadResult {
    Applied,
    RestartRequired,
    Failed,
};

class Server {
public:
    using StartupCallback = std::function<void(bool, const std::string&)>;
    using LogSinkFactory = std::function<std::unique_ptr<LogSink>()>;

    explicit Server(
        RuntimeSnapshotPtr snapshot,
        LogSinkFactory log_sink_factory = {},
        bool handle_process_signals = true);
    ~Server();

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
    void log_request_error(
        const std::shared_ptr<RequestGeneration>& generation,
        int status_code,
        const std::string& type,
        const std::string& message) const;
    bool process_with_generation(
        const std::shared_ptr<RequestGeneration>& generation,
        const std::string& raw,
        const std::string& client_ip,
        const std::function<bool(const std::string&)>& sender,
        const CancellationToken& cancellation) const;
    HttpResponse handle_local_route_error(const RouteLookup& route) const;
    std::pair<HttpResponse, bool> handle_usage_request(
        const std::shared_ptr<RequestGeneration>& generation,
        const HttpRequest& request,
        const RouteEntry& route,
        const CancellationToken& cancellation) const;
    std::shared_ptr<Logger> make_logger(const RuntimeSnapshot& snapshot);
    void handle_logger_failure(const std::string& error) noexcept;
    void log_performance_snapshot(const std::string& reason) const;

    std::shared_ptr<RuntimeMetrics> metrics_;
    std::shared_ptr<InflightMemoryBudget> inflight_budget_;
    LogSinkFactory log_sink_factory_;
    mutable std::shared_ptr<RequestGeneration> generation_;
    mutable std::shared_mutex generation_mutex_;
    mutable std::mutex reload_mutex_;
    std::mutex client_shutdown_mutex_;
    std::function<void()> client_shutdown_;
    bool handle_process_signals_ = true;
    std::atomic_bool fatal_error_{false};
    std::atomic_bool stop_requested_{false};
};

} // namespace ccs
