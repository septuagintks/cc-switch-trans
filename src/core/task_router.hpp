#pragma once

#include "config/config.hpp"

#include <string>
#include <vector>

namespace ccs {

struct RouteDecision {
    const EndpointGroupConfig* endpoint = nullptr;
    const TaskConfig* task = nullptr;

    bool configured() const;
    UpstreamTarget upstream_target() const;
};

class TaskRouter {
public:
    explicit TaskRouter(std::vector<const EndpointGroupConfig*> endpoints);

    RouteDecision route(const std::string& path) const;

private:
    std::vector<const EndpointGroupConfig*> endpoints_;
};

} // namespace ccs
