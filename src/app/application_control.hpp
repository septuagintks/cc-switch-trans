#pragma once

#include "app/application_status.hpp"

#include <string>

namespace ccs {

class ApplicationControl {
public:
    virtual ~ApplicationControl() = default;

    virtual bool start(std::string& error) = 0;
    virtual bool reload(std::string& error) = 0;
    virtual bool stop(std::string& error) = 0;
    virtual bool shutdown(std::string& error) = 0;
    virtual ApplicationStatus status() = 0;
};

} // namespace ccs
