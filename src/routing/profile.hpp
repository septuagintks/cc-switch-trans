#pragma once

#include "config/config_document.hpp"

#include <string>
#include <vector>

namespace ccs {

struct RuntimeProfile {
    std::string id;
    ProtocolId protocol;
    bool source_enabled = false;
    std::vector<RuleDefinition> request_pipeline;
};

} // namespace ccs
