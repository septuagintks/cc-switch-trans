#include "protocols/responses_handler.hpp"

namespace ccs {

const ProtocolDescriptor& ResponsesHandler::descriptor() const noexcept {
    static const ProtocolDescriptor value{
        "responses",
        "POST",
        "GET",
        true,
        true,
        true,
        ProtocolErrorEnvelope::OpenAI,
        {"remove_tool"},
    };
    return value;
}

} // namespace ccs
