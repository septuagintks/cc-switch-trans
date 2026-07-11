#pragma once

#include "config/config_document.hpp"
#include "protocols/protocol_handler.hpp"

#include <memory>
#include <string>
#include <vector>

namespace ccs {

struct RuntimeProfile {
    std::string id;
    std::shared_ptr<const ProtocolHandler> handler;
    bool source_enabled = false;
    std::vector<RuleDefinition> request_pipeline;
};

} // namespace ccs
