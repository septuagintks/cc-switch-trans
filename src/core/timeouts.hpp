#pragma once

namespace ccs {

struct TimeoutConfig {
    int resolve_ms = 300000;
    int connect_ms = 300000;
    int send_ms = 300000;
    int response_header_ms = 300000;
    int stream_idle_ms = 300000;
    int total_ms = 0;

    bool operator==(const TimeoutConfig&) const = default;
};

} // namespace ccs
