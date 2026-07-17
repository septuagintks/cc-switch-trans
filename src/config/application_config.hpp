#pragma once

#include "config/application_settings.hpp"

#include <string>
#include <string_view>

namespace ccs {

inline constexpr std::string_view kApplicationConfigSchema = "ccs-trans.config/v3";

enum class ConfigSchemaKind {
    V2,
    V3,
    Unsupported,
};

struct ApplicationConfigDocument {
    ApplicationSettings application;

    bool operator==(const ApplicationConfigDocument&) const = default;
};

ApplicationConfigDocument make_default_application_config_document();

bool detect_config_schema(
    std::string_view content,
    ConfigSchemaKind& schema,
    std::string& error);
bool parse_application_config_document(
    std::string_view content,
    ApplicationConfigDocument& document,
    std::string& error);
bool serialize_application_config_document(
    const ApplicationConfigDocument& document,
    std::string& content,
    std::string& error);
bool validate_application_config_document(
    const ApplicationConfigDocument& document,
    std::string& error);

} // namespace ccs
