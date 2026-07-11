#include "protocols/chat_handler.hpp"

namespace ccs {

const ProtocolDescriptor& ChatHandler::descriptor() const noexcept {
    static const ProtocolDescriptor value{
        "chat",
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
