#pragma once

#include "config/config.hpp"
#include "core/http_types.hpp"
#include "core/task_router.hpp"
#include "logging/logger.hpp"
#include "transport/proxy.hpp"
#include "transforms/findcg_responses_transform.hpp"

#include <functional>
#include <string>

namespace ccs {

class Server {
public:
    explicit Server(AppConfig config);

    int run();
    void request_stop();
    std::string process_raw_request(const std::string& raw, const std::string& client_ip) const;
    bool process_raw_request_to_sender(
        const std::string& raw,
        const std::string& client_ip,
        const std::function<bool(const std::string&)>& sender) const;
    void log_request_error(int status_code, const std::string& type, const std::string& message) const;

private:
    HttpResponse handle_local_route_error(const HttpRequest& request) const;
    HttpResponse handle_usage_request(const HttpRequest& request, const TaskConfig& task) const;

    AppConfig config_;
    TaskRouter router_;
    Proxy proxy_;
    mutable Logger logger_;
    FindcgResponsesTransform findcg_transform_;
};

} // namespace ccs
