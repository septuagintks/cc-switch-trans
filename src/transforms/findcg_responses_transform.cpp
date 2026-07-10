#include "transforms/findcg_responses_transform.hpp"

#include "core/url.hpp"

#include <nlohmann/json.hpp>

#include <utility>

namespace ccs {

namespace {

std::string string_field(const nlohmann::json& value, const char* name) {
    const auto it = value.find(name);
    if (it == value.end() || !it->is_string()) {
        return {};
    }
    return it->get<std::string>();
}

bool should_remove(const nlohmann::json& tool) {
    if (!tool.is_object()) {
        return false;
    }
    const auto name = string_field(tool, "name");
    const auto type = string_field(tool, "type");
    const auto tool_namespace = string_field(tool, "namespace");
    return (type == "namespace" && name == "image_gen")
        || name == "image_gen"
        || tool_namespace == "image_gen";
}

} // namespace

TransformResult FindcgResponsesTransform::apply(const TaskConfig& task, const std::string& body) const {
    TransformResult result;
    result.rewrite_name = "remove_findcg_image_gen";
    result.original_body_size = body.size();
    result.rewritten_body_size = body.size();

    if (task.kind != ApiTaskKind::Responses) {
        result.rewrite_reason = "task_not_responses";
        return result;
    }

    ParsedUrl upstream;
    try {
        upstream = parse_http_url(task.upstream.base_url);
    } catch (const std::exception& ex) {
        throw TransformError(500, "configuration_error", ex.what());
    }
    if (!is_findcg_host(upstream.host)) {
        result.rewrite_reason = "upstream_not_findcg";
        return result;
    }

    result.matched = true;
    result.rewrite_reason = "findcg_responses";

    nlohmann::json root;
    try {
        root = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        throw TransformError(400, "invalid_request_error", "findcg Responses body is not valid JSON");
    }

    if (!root.is_object()) {
        throw TransformError(400, "invalid_request_error", "findcg Responses body must be a JSON object");
    }

    const auto tools_it = root.find("tools");
    if (tools_it == root.end() || !tools_it->is_array()) {
        return result;
    }

    nlohmann::json filtered = nlohmann::json::array();
    for (const auto& tool : *tools_it) {
        if (!should_remove(tool)) {
            filtered.push_back(tool);
            continue;
        }
        result.removed_tools.push_back(RemovedTool{
            string_field(tool, "type"),
            string_field(tool, "name").empty() ? string_field(tool, "namespace") : string_field(tool, "name"),
        });
    }

    if (result.removed_tools.empty()) {
        return result;
    }

    root["tools"] = std::move(filtered);
    try {
        result.rewritten_body = root.dump();
    } catch (const nlohmann::json::exception&) {
        throw TransformError(500, "server_error", "failed to serialize rewritten Responses body");
    }
    result.modified = true;
    result.rewritten_body_size = result.rewritten_body->size();
    return result;
}

} // namespace ccs
