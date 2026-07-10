#include "core/task_router.hpp"

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

TaskRouter::TaskRouter(const AppConfig& config)
    : config_(config) {}

RouteDecision TaskRouter::route(const std::string& path) const {
    if (path_matches(path, config_.responses)) {
        return RouteDecision{&config_.responses};
    }
    if (path_matches(path, config_.chat_completions)) {
        return RouteDecision{&config_.chat_completions};
    }
    if (path_matches(path, config_.usage)) {
        return RouteDecision{&config_.usage};
    }
    return {};
}

} // namespace ccs
