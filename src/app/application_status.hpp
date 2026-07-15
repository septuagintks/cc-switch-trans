#pragma once

#include <cstdint>
#include <string>

namespace ccs {

enum class ApplicationState {
    Stopped,
    Starting,
    Running,
    Reloading,
    Stopping,
    Faulted,
    Shutdown,
};

struct ApplicationStatus {
    ApplicationState state = ApplicationState::Stopped;
    std::string listener_host;
    std::uint16_t listener_port = 0;
    std::string last_error;
    int last_exit_code = 0;
};

const char* application_state_name(ApplicationState state);

} // namespace ccs
