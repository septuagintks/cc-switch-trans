#pragma once

#include "config/app_paths.hpp"
#include "presentation/ui_preferences_repository.hpp"

#include <string>

namespace ccs {

class UiPreferencesStore final : public UiPreferencesRepository {
public:
    explicit UiPreferencesStore(AppPaths paths);

    bool load(UiPreferences& preferences, std::string& error) override;
    bool save(const UiPreferences& preferences, std::string& error) override;

    bool loaded() const noexcept;
    const AppPaths& paths() const noexcept;

private:
    bool source_is_unchanged(std::string& error) const;

    AppPaths paths_;
    std::string source_content_;
    bool source_exists_ = false;
    bool loaded_ = false;
};

} // namespace ccs
