#pragma once

#include "config/app_paths.hpp"
#include "config/config_document.hpp"

#include <string>

namespace ccs {

enum class ConfigRepositoryFailure {
    None,
    NotLoaded,
    Busy,
    Stale,
    InvalidDocument,
    Io,
};

class ConfigRepository {
public:
    virtual ~ConfigRepository() = default;

    virtual bool load(std::string& error) = 0;
    virtual bool save(const ConfigDocument& document, std::string& error) = 0;
    virtual bool loaded() const = 0;
    virtual const ConfigDocument& document() const = 0;
    virtual const AppPaths& paths() const = 0;
    virtual ConfigRepositoryFailure last_failure() const noexcept {
        return ConfigRepositoryFailure::None;
    }
};

} // namespace ccs
