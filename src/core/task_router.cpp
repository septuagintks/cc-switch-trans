#include "core/task_router.hpp"

#include <utility>

namespace ccs {

namespace {

std::string canonical_route_path(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

bool path_matches(const std::string& actual, const TaskConfig& task) {
    return canonical_route_path(actual) == canonical_route_path(task.local_path);
}

} // namespace

bool RouteDecision::configured() const {
    return endpoint != nullptr && task != nullptr && endpoint->enabled();
}

UpstreamTarget RouteDecision::upstream_target() const {
    if (endpoint == nullptr || task == nullptr) {
        return {};
    }
    return endpoint->upstream_target(*task);
}

TaskRouter::TaskRouter(std::vector<const EndpointGroupConfig*> endpoints)
    : endpoints_(std::move(endpoints)) {}

RouteDecision TaskRouter::route(const std::string& path) const {
    for (const auto* endpoint : endpoints_) {
        if (endpoint == nullptr) {
            continue;
        }
        if (path_matches(path, endpoint->main_task)) {
            return RouteDecision{endpoint, &endpoint->main_task};
        }
        if (path_matches(path, endpoint->usage_task)) {
            return RouteDecision{endpoint, &endpoint->usage_task};
        }
    }
    return {};
}

} // namespace ccs
