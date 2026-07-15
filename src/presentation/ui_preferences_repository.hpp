#pragma once

#include "presentation/ui_preferences.hpp"

#include <string>

namespace ccs {

class UiPreferencesRepository {
public:
    virtual ~UiPreferencesRepository() = default;

    virtual bool load(UiPreferences& preferences, std::string& error) = 0;
    virtual bool save(const UiPreferences& preferences, std::string& error) = 0;
};

} // namespace ccs
