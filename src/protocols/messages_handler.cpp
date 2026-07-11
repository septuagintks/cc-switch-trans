#include "protocols/messages_handler.hpp"

namespace ccs {

const ProtocolDescriptor& MessagesHandler::descriptor() const noexcept {
    static const ProtocolDescriptor value{
        "messages",
        "POST",
        "GET",
        true,
        true,
        true,
        ProtocolErrorEnvelope::Anthropic,
        {"remove_tool"},
    };
    return value;
}

} // namespace ccs
