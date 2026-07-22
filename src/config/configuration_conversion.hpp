#pragma once

#include "config/config_document.hpp"
#include "config/configuration_snapshot.hpp"

#include <string>
#include <vector>

namespace ccs {

bool config_document_to_stored_profiles(
    const ConfigDocument& document,
    std::vector<StoredProfile>& profiles,
    std::string& error);
bool configuration_snapshot_to_config_document(
    const ConfigurationSnapshot& snapshot,
    ConfigDocument& document,
    std::string& error);
bool configuration_snapshot_to_config_document(
    const ConfigurationSnapshot& snapshot,
    ConfigDocument& document,
    ConfigDocumentValidationFailure& failure);

} // namespace ccs
