#include "core/task.hpp"

namespace ccs {

bool EndpointGroupConfig::enabled() const {
    return !upstream_url.empty();
}

UpstreamTarget EndpointGroupConfig::upstream_target(const TaskConfig& task) const {
    return UpstreamTarget{upstream_url, task.upstream_path};
}

const char* endpoint_group_name(EndpointGroupKind kind) {
    switch (kind) {
    case EndpointGroupKind::Responses:
        return "responses";
    case EndpointGroupKind::Chat:
        return "chat";
    }
    return "unknown";
}

const char* task_name(ApiTaskKind kind) {
    switch (kind) {
    case ApiTaskKind::Responses:
        return "responses";
    case ApiTaskKind::ResponsesUsage:
        return "responses_usage";
    case ApiTaskKind::ChatCompletions:
        return "chat_completions";
    case ApiTaskKind::ChatUsage:
        return "chat_usage";
    }
    return "unknown";
}

bool is_usage_task(ApiTaskKind kind) {
    return kind == ApiTaskKind::ResponsesUsage || kind == ApiTaskKind::ChatUsage;
}

} // namespace ccs
