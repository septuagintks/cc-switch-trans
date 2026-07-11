#include "routing/route_table.hpp"

#include "core/url.hpp"

#include <algorithm>
#include <functional>
#include <utility>

namespace ccs {

namespace {

bool valid_method(const std::string& method) {
    if (method.empty() || method.size() > 32) {
        return false;
    }
    return std::all_of(method.begin(), method.end(), [](unsigned char ch) {
        return (ch >= 'A' && ch <= 'Z')
            || (ch >= '0' && ch <= '9')
            || ch == '-';
    });
}

} // namespace

const char* route_kind_name(RouteKind kind) {
    return kind == RouteKind::Usage ? "usage" : "request";
}

bool RouteKey::operator==(const RouteKey& other) const {
    return method == other.method && canonical_path == other.canonical_path;
}

std::size_t RouteKeyHash::operator()(const RouteKey& key) const noexcept {
    const auto method_hash = std::hash<std::string>{}(key.method);
    const auto path_hash = std::hash<std::string>{}(key.canonical_path);
    return method_hash ^ (path_hash + static_cast<std::size_t>(0x9e3779b9U)
        + (method_hash << 6U) + (method_hash >> 2U));
}

bool RouteTable::add(RouteEntry entry, std::string& error) {
    error.clear();
    if (!entry.profile) {
        error = "route must reference an immutable runtime profile";
        return false;
    }
    if (!valid_method(entry.method)) {
        error = "route method must be an uppercase HTTP token";
        return false;
    }
    std::string canonical;
    if (!canonicalize_http_path(entry.local_path, canonical, error)) {
        error = "invalid local route for profile " + entry.profile->id + ": " + error;
        return false;
    }
    if (canonical == "/_ccs-trans" || canonical.rfind("/_ccs-trans/", 0) == 0) {
        error = "profile " + entry.profile->id + " uses the reserved /_ccs-trans management namespace";
        return false;
    }
    if (entry.upstream.base_url.empty() || entry.upstream.path.empty()) {
        error = "route for profile " + entry.profile->id + " has an incomplete upstream target";
        return false;
    }

    entry.local_path = canonical;
    const auto method = entry.method;
    auto& path_routes = routes_by_path_[canonical];
    const auto existing = path_routes.by_method.find(method);
    if (existing != path_routes.by_method.end()) {
        error = "route collision for " + method + " " + canonical
            + " between profiles " + existing->second.profile->id
            + " and " + entry.profile->id;
        return false;
    }
    path_routes.allowed_methods.push_back(method);
    std::sort(path_routes.allowed_methods.begin(), path_routes.allowed_methods.end());
    path_routes.by_method.emplace(method, std::move(entry));
    ++size_;
    return true;
}

RouteLookup RouteTable::lookup(const std::string& method, const std::string& path) const {
    RouteLookup result;
    if (!canonicalize_http_path(path, result.canonical_path, result.error)) {
        result.status = RouteLookupStatus::InvalidPath;
        return result;
    }
    const auto path_routes = routes_by_path_.find(result.canonical_path);
    if (path_routes == routes_by_path_.end()) {
        result.status = RouteLookupStatus::NotFound;
        return result;
    }
    const auto route = path_routes->second.by_method.find(method);
    if (route != path_routes->second.by_method.end()) {
        result.status = RouteLookupStatus::Matched;
        result.entry = &route->second;
        return result;
    }
    result.status = RouteLookupStatus::MethodNotAllowed;
    result.allowed_methods = path_routes->second.allowed_methods;
    return result;
}

std::size_t RouteTable::size() const {
    return size_;
}

bool RouteTable::empty() const {
    return size_ == 0;
}

} // namespace ccs
