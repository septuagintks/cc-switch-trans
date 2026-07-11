#pragma once

#include "core/http_types.hpp"
#include "routing/profile.hpp"

#include <cstddef>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace ccs {

enum class RouteKind {
    Request,
    Usage,
};

const char* route_kind_name(RouteKind kind);

struct RouteKey {
    std::string method;
    std::string canonical_path;

    bool operator==(const RouteKey& other) const;
};

struct RouteKeyHash {
    std::size_t operator()(const RouteKey& key) const noexcept;
};

struct RouteEntry {
    std::shared_ptr<const RuntimeProfile> profile;
    RouteKind kind = RouteKind::Request;
    std::string method;
    std::string local_path;
    UpstreamTarget upstream;
};

enum class RouteLookupStatus {
    Matched,
    MethodNotAllowed,
    NotFound,
    InvalidPath,
};

struct RouteLookup {
    RouteLookupStatus status = RouteLookupStatus::NotFound;
    const RouteEntry* entry = nullptr;
    std::string canonical_path;
    std::vector<std::string> allowed_methods;
    std::string error;
};

class RouteTable {
public:
    bool add(RouteEntry entry, std::string& error);
    RouteLookup lookup(const std::string& method, const std::string& path) const;

    std::size_t size() const;
    bool empty() const;

private:
    struct PathRoutes {
        std::unordered_map<std::string, RouteEntry> by_method;
        std::vector<std::string> allowed_methods;
    };

    std::unordered_map<std::string, PathRoutes> routes_by_path_;
    std::size_t size_ = 0;
};

} // namespace ccs
