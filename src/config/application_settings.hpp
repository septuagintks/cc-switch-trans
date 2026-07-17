#pragma once

#include "core/inflight_memory_budget.hpp"
#include "core/timeouts.hpp"

#include <cstdint>
#include <string>

namespace ccs {

inline constexpr std::uint64_t kDefaultLogMaxTotalSize =
    2ULL * 1024 * 1024 * 1024;

struct ListenerSettings {
    std::string host = "127.0.0.1";
    std::uint16_t port = 15723;

    bool operator==(const ListenerSettings&) const = default;
};

struct RuntimeSettings {
    std::uint32_t worker_threads = 32;
    std::uint32_t max_connections = 64;
    std::uint64_t max_request_body_size = 100ULL * 1024 * 1024;
    std::uint64_t max_response_body_size = 100ULL * 1024 * 1024;
    std::uint64_t max_inflight_bytes = kDefaultInflightMemoryBudget;
    std::uint32_t metrics_interval_ms = 0;

    bool operator==(const RuntimeSettings&) const = default;
};

struct LoggingSettings {
    std::string path = "logs/ccs-trans.log";
    std::string level = "info";
    bool body = true;
    bool redact_sensitive = false;
    std::uint64_t body_limit = 1024ULL * 1024;
    std::uint64_t queue_capacity = 16ULL * 1024 * 1024;
    std::uint64_t max_total_size = kDefaultLogMaxTotalSize;
    std::uint32_t flush_interval_ms = 100;

    bool operator==(const LoggingSettings&) const = default;
};

struct ApplicationSettings {
    ListenerSettings listener;
    RuntimeSettings runtime;
    TimeoutConfig timeouts;
    LoggingSettings logging;

    bool operator==(const ApplicationSettings&) const = default;
};

} // namespace ccs
