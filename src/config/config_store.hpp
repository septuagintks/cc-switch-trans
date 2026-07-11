#pragma once

#include "config/app_paths.hpp"
#include "config/config_document.hpp"

#include <string>

namespace ccs {

class ConfigStore {
public:
    explicit ConfigStore(AppPaths paths);

    bool load(std::string& error);
    bool save(const ConfigDocument& document, std::string& error);

    bool loaded() const;
    const ConfigDocument& document() const;
    const AppPaths& paths() const;

private:
    bool source_is_unchanged(std::string& error) const;

    AppPaths paths_;
    ConfigDocument document_;
    std::string source_content_;
    bool source_exists_ = false;
    bool loaded_ = false;
};

} // namespace ccs
