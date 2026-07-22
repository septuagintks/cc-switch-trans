#pragma once

#include "config/application_settings.hpp"
#include "config/profile_model.hpp"

#include <string>
#include <vector>

namespace ccs {

struct ApplicationSourceToken {
    bool exists = false;
    std::string bytes;

    bool operator==(const ApplicationSourceToken&) const = default;
};

struct RepositoryRevision {
    ApplicationSourceToken application_source;
    ProfileRevision profile_revision = 0;

    bool operator==(const RepositoryRevision&) const = default;
};

std::string repository_revision_token(const RepositoryRevision& revision);

struct ConfigurationSnapshot {
    ApplicationSettings application;
    std::vector<StoredProfile> profiles;
    RepositoryRevision revision;
    std::optional<std::string> migrated_from_sha256;

    bool operator==(const ConfigurationSnapshot&) const = default;
};

} // namespace ccs
