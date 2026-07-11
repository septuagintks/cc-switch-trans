#pragma once

#include "protocols/protocol_handler.hpp"
#include "rules/rule.hpp"

#include <memory>
#include <string>

namespace ccs {

struct RuntimeProfile {
    std::string id;
    std::shared_ptr<const ProtocolHandler> handler;
    bool source_enabled = false;
    std::shared_ptr<const CompiledPipeline> request_pipeline;
};

} // namespace ccs
