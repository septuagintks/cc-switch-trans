#pragma once

#include "config/config.hpp"

#include <string>

namespace ccs {

struct RouteDecision {
    const TaskConfig* task = nullptr;
};

class TaskRouter {
public:
    explicit TaskRouter(const AppConfig& config);

    RouteDecision route(const std::string& path) const;

private:
    const AppConfig& config_;
};

} // namespace ccs
