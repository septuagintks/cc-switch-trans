#include "protocols/protocol_handler.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace ccs {

namespace {

std::string reason_phrase(int status_code) {
    switch (status_code) {
    case 400:
        return "Bad Request";
    case 404:
        return "Not Found";
    case 405:
        return "Method Not Allowed";
    case 413:
        return "Payload Too Large";
    case 429:
        return "Too Many Requests";
    case 500:
        return "Internal Server Error";
    case 502:
        return "Bad Gateway";
    case 503:
        return "Service Unavailable";
    case 504:
        return "Gateway Timeout";
    default:
        return "Error";
    }
}

} // namespace

std::string_view ProtocolHandler::id() const noexcept {
    return descriptor().id;
}

bool ProtocolHandler::validate_profile(
    const std::string& profile_id,
    const ProfileDefinition& profile,
    std::string& error) const {
    if (!validate_profile_definition(profile_id, profile, true, error)) {
        return false;
    }
    if (!profile.protocol || profile.protocol->value != descriptor().id) {
        error = "profile " + profile_id + " protocol does not match handler " + descriptor().id;
        return false;
    }
    const bool has_usage = profile.local.usage_path.has_value();
    if (has_usage && !descriptor().supports_usage) {
        error = "protocol " + descriptor().id + " does not support a Usage route";
        return false;
    }
    return true;
}

bool ProtocolHandler::supports_specialized_rule(std::string_view rule_type) const {
    const auto& types = descriptor().specialized_rule_types;
    return std::find(types.begin(), types.end(), rule_type) != types.end();
}

HttpResponse ProtocolHandler::local_error(
    int status_code,
    std::string type,
    std::string message) const {
    nlohmann::json body = nlohmann::json::object();
    if (descriptor().error_envelope == ProtocolErrorEnvelope::Anthropic) {
        body["type"] = "error";
        body["error"] = {
            {"type", std::move(type)},
            {"message", std::move(message)},
        };
    } else {
        body["error"] = {
            {"message", std::move(message)},
            {"type", std::move(type)},
        };
    }
    HttpResponse response;
    response.status_code = status_code;
    response.reason = reason_phrase(status_code);
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = body.dump();
    return response;
}

} // namespace ccs
