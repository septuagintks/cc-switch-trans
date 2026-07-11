#pragma once

#include "core/http_types.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace ccs {

enum class EndpointGroupKind {
    Responses,
    Chat,
};

enum class ApiTaskKind {
    Responses,
    ResponsesUsage,
    ChatCompletions,
    ChatUsage,
};

struct TaskConfig {
    ApiTaskKind kind = ApiTaskKind::Responses;
    std::string method;
    std::string local_path;
    std::string upstream_path;
    std::vector<std::string> transforms;
    bool log_request_chain = true;
};

struct EndpointGroupConfig {
    EndpointGroupKind kind = EndpointGroupKind::Responses;
    std::string listen_host = "127.0.0.1";
    std::uint16_t listen_port = 0;
    std::string upstream_url;
    TaskConfig main_task;
    TaskConfig usage_task;

    bool enabled() const;
    UpstreamTarget upstream_target(const TaskConfig& task) const;
};

const char* endpoint_group_name(EndpointGroupKind kind);
const char* task_name(ApiTaskKind kind);
bool is_usage_task(ApiTaskKind kind);

} // namespace ccs
