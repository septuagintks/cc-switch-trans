#pragma once

#include "config/config_document.hpp"
#include "core/http_types.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ccs {

enum class ProtocolErrorEnvelope {
    OpenAI,
    Anthropic,
};

struct ProtocolDescriptor {
    std::string id;
    std::string request_method;
    std::string usage_method;
    bool supports_usage = false;
    bool supports_sse = false;
    bool request_body_is_json = true;
    ProtocolErrorEnvelope error_envelope = ProtocolErrorEnvelope::OpenAI;
    std::vector<std::string> specialized_rule_types;
};

class ProtocolHandler {
public:
    virtual ~ProtocolHandler() = default;

    virtual const ProtocolDescriptor& descriptor() const noexcept = 0;

    std::string_view id() const noexcept;
    bool validate_profile(
        const std::string& profile_id,
        const ProfileDefinition& profile,
        std::string& error) const;
    bool supports_specialized_rule(std::string_view rule_type) const;
    HttpResponse local_error(int status_code, std::string type, std::string message) const;
};

} // namespace ccs
