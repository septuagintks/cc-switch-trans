#pragma once

#include "config/application_settings.hpp"
#include "config/profile_model.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace ccs {

enum class ConfigurationFieldScope {
    Application,
    Profile,
};

enum class ConfigurationFieldInputKind {
    Text,
    UnsignedInteger,
    Boolean,
    Enumeration,
    Path,
    Url,
};

enum class RuntimeApplyImpact {
    RuntimeReload,
    ServiceRestart,
};

struct ConfigurationFieldDescriptor {
    std::string_view key;
    ConfigurationFieldScope scope = ConfigurationFieldScope::Application;
    ConfigurationFieldInputKind input_kind = ConfigurationFieldInputKind::Text;
    bool required = true;
    std::optional<std::uint64_t> minimum;
    std::optional<std::uint64_t> maximum;
    std::vector<std::string_view> enum_values;
    std::string_view display_name_key;
    RuntimeApplyImpact apply_impact = RuntimeApplyImpact::RuntimeReload;
};

using ConfigurationFieldValue = std::variant<std::string, std::uint64_t, bool>;

std::span<const ConfigurationFieldDescriptor> application_field_descriptors();
std::span<const ConfigurationFieldDescriptor> profile_field_descriptors();
const ConfigurationFieldDescriptor* find_configuration_field_descriptor(
    ConfigurationFieldScope scope,
    std::string_view key);

bool parse_configuration_field_value(
    const ConfigurationFieldDescriptor& descriptor,
    std::string_view raw,
    ConfigurationFieldValue& value,
    std::string& error);
bool apply_application_field(
    ApplicationSettings& application,
    const ConfigurationFieldDescriptor& descriptor,
    const ConfigurationFieldValue& value,
    std::string& error);
bool reset_application_field(
    ApplicationSettings& application,
    const ConfigurationFieldDescriptor& descriptor,
    std::string& error);
bool apply_profile_field(
    StoredProfile& profile,
    const ConfigurationFieldDescriptor& descriptor,
    const ConfigurationFieldValue& value,
    std::string& error);
bool reset_profile_field(
    StoredProfile& profile,
    const ConfigurationFieldDescriptor& descriptor,
    std::string& error);

const char* configuration_field_input_kind_name(
    ConfigurationFieldInputKind kind) noexcept;
const char* runtime_apply_impact_name(RuntimeApplyImpact impact) noexcept;

} // namespace ccs
