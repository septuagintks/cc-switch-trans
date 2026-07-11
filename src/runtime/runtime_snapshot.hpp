#pragma once

#include "config/config_document.hpp"
#include "protocols/protocol_registry.hpp"
#include "routing/profile.hpp"
#include "routing/route_table.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>

namespace ccs {

struct RuntimeSnapshot {
    ApplicationSettings application;
    std::filesystem::path log_path;
    std::shared_ptr<const ProtocolRegistry> protocols;
    std::map<std::string, std::shared_ptr<const RuntimeProfile>> profiles;
    RouteTable routes;
};

using RuntimeSnapshotPtr = std::shared_ptr<const RuntimeSnapshot>;

} // namespace ccs
