#pragma once

#include "protocols/protocol_handler.hpp"

namespace ccs {

class MessagesHandler final : public ProtocolHandler {
public:
    const ProtocolDescriptor& descriptor() const noexcept override;
};

} // namespace ccs
