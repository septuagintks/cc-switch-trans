#pragma once

#include <string>

namespace ccs {

class MacStartupRegistration {
public:
    bool status(
        bool& enabled,
        bool& requires_approval,
        std::string& error) const;
    bool set_registered(bool enabled, std::string& error) const;
};

} // namespace ccs
