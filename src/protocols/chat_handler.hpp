#pragma once

#include "protocols/protocol_handler.hpp"

namespace ccs {

class ChatHandler final : public ProtocolHandler {
public:
    const ProtocolDescriptor& descriptor() const noexcept override;
};

} // namespace ccs
