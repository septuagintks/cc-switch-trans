#pragma once

#include "config/config.hpp"
#include "core/cancellation.hpp"
#include "core/http_types.hpp"
#include "core/runtime_metrics.hpp"
#include "core/task_router.hpp"
#include "logging/logger.hpp"
#include "transport/proxy.hpp"
#include "transforms/findcg_responses_transform.hpp"

#include <functional>
#include <memory>
#include <string>

namespace ccs {

class Server {
public:
    using StartupCallback = std::function<void(bool, const std::string&)>;

    explicit Server(AppConfig config);

    int run(const StartupCallback& startup_callback = {});
    void request_stop();
    std::string process_raw_request(
        const std::string& raw,
        const std::string& client_ip,
        EndpointGroupKind endpoint = EndpointGroupKind::Responses) const;
    bool process_raw_request_to_sender(
        const std::string& raw,
        const std::string& client_ip,
        EndpointGroupKind endpoint,
        const std::function<bool(const std::string&)>& sender,
        const CancellationToken& cancellation = {}) const;
    void log_request_error(
        EndpointGroupKind endpoint,
        int status_code,
        const std::string& type,
        const std::string& message) const;

private:
    HttpResponse handle_local_route_error(const HttpRequest& request, const RouteDecision& route) const;
    HttpResponse handle_usage_request(
        const HttpRequest& request,
        const EndpointGroupConfig& endpoint,
        const TaskConfig& task,
        const CancellationToken& cancellation) const;
    void log_performance_snapshot(const std::string& reason) const;

    AppConfig config_;
    std::shared_ptr<RuntimeMetrics> metrics_;
    TaskRouter responses_router_;
    TaskRouter chat_router_;
    Proxy proxy_;
    mutable Logger logger_;
    FindcgResponsesTransform findcg_transform_;
};

} // namespace ccs
