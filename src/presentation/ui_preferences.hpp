#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace ccs {

inline constexpr std::size_t kMaxUiPreferencesBytes = 64 * 1024;
inline constexpr std::string_view kUiPreferencesSchemaVersion = "ccs-trans.ui/v1";

struct UiPreferences {
    bool lightweight_mode = true;

    bool operator==(const UiPreferences&) const = default;
};

UiPreferences make_default_ui_preferences();
bool parse_ui_preferences(
    std::string_view content,
    UiPreferences& preferences,
    std::string& error);
bool serialize_ui_preferences(
    const UiPreferences& preferences,
    std::string& content,
    std::string& error);

} // namespace ccs
