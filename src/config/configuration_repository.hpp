#pragma once

#include "config/app_paths.hpp"
#include "config/config_repository.hpp"
#include "config/configuration_snapshot.hpp"

#include <string>

namespace ccs {

class ConfigurationRepository {
public:
    virtual ~ConfigurationRepository() = default;

    virtual bool load(std::string& error) = 0;
    virtual bool loaded() const = 0;
    virtual const ConfigurationSnapshot& snapshot() const = 0;
    virtual bool save_snapshot(
        const ConfigurationSnapshot& desired,
        ConfigurationSnapshot& committed,
        std::string& error) = 0;
    virtual const AppPaths& paths() const = 0;
    virtual ConfigRepositoryFailure last_failure() const noexcept = 0;
};

} // namespace ccs
