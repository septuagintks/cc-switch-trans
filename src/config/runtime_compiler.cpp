#include "config/runtime_compiler.hpp"

#include <memory>
#include <utility>
#include <vector>

namespace ccs {

RuntimeCompiler::RuntimeCompiler(std::filesystem::path application_root)
    : application_root_(std::move(application_root)) {}

bool RuntimeCompiler::compile(
    const ConfigDocument& document,
    const RuntimeCompileOptions& options,
    RuntimeSnapshotPtr& snapshot,
    std::string& error) const {
    error.clear();
    if (application_root_.empty() || !application_root_.is_absolute()) {
        error = "runtime compiler requires an absolute application root";
        return false;
    }
    if (!validate_config_document(document, error)) {
        return false;
    }

    std::vector<std::pair<std::string, const ProfileDefinition*>> selected;
    if (options.selected_profile) {
        const auto profile = document.profiles.find(*options.selected_profile);
        if (profile == document.profiles.end()) {
            error = "selected profile does not exist: " + *options.selected_profile;
            return false;
        }
        if (!validate_profile_definition(profile->first, profile->second, true, error)) {
            error = "selected profile is not runnable: " + error;
            return false;
        }
        selected.emplace_back(profile->first, &profile->second);
    } else {
        for (const auto& [profile_id, profile] : document.profiles) {
            if (profile.enabled) {
                selected.emplace_back(profile_id, &profile);
            }
        }
    }
    if (selected.empty()) {
        error = "no enabled profiles; enable a complete profile or use run --profile";
        return false;
    }

    RuntimeSnapshot candidate;
    candidate.application = document.application;
    if (!resolve_application_log_path(
            document.application,
            application_root_,
            candidate.log_path,
            error)) {
        return false;
    }

    for (const auto& [profile_id, definition] : selected) {
        auto mutable_profile = std::make_shared<RuntimeProfile>();
        mutable_profile->id = profile_id;
        mutable_profile->protocol = *definition->protocol;
        mutable_profile->source_enabled = definition->enabled;
        for (const auto& rule : definition->rules) {
            if (rule.enabled) {
                mutable_profile->request_pipeline.push_back(rule);
            }
        }
        std::shared_ptr<const RuntimeProfile> profile = std::move(mutable_profile);
        candidate.profiles.emplace(profile_id, profile);

        RouteEntry request_route;
        request_route.profile = profile;
        request_route.kind = RouteKind::Request;
        request_route.method = "POST";
        request_route.local_path = *definition->local.request_path;
        request_route.upstream = UpstreamTarget{
            *definition->upstream.base_url,
            *definition->upstream.request_path,
        };
        if (!candidate.routes.add(std::move(request_route), error)) {
            return false;
        }

        if (definition->local.usage_path) {
            RouteEntry usage_route;
            usage_route.profile = profile;
            usage_route.kind = RouteKind::Usage;
            usage_route.method = "GET";
            usage_route.local_path = *definition->local.usage_path;
            usage_route.upstream = UpstreamTarget{
                *definition->upstream.base_url,
                *definition->upstream.usage_path,
            };
            if (!candidate.routes.add(std::move(usage_route), error)) {
                return false;
            }
        }
    }

    snapshot = std::make_shared<const RuntimeSnapshot>(std::move(candidate));
    return true;
}

} // namespace ccs
