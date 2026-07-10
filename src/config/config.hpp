#pragma once

#include "core/task.hpp"
#include "core/timeouts.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>

namespace ccs {

struct AppConfig {
    std::string listen_host = "127.0.0.1";
    std::uint16_t listen_port = 15723;

    TaskConfig responses{
        ApiTaskKind::Responses,
        false,
        "POST",
        "/v1/responses/",
        {"", "/v1/responses/"},
        {"remove_findcg_image_gen"},
        true,
    };
    TaskConfig chat_completions{
        ApiTaskKind::ChatCompletions,
        false,
        "POST",
        "/v1/chat/completions",
        {"", "/v1/chat/completions"},
        {},
        true,
    };
    TaskConfig usage{
        ApiTaskKind::Usage,
        false,
        "GET",
        "/v1/usage",
        {"", "/v1/usage"},
        {},
        false,
    };

    std::filesystem::path log_path = "./logs/ccs-trans.log";
    std::string log_level = "info";
    bool log_body = true;
    bool redact_sensitive = false;
    std::size_t body_log_limit = 1024 * 1024;
    std::size_t log_queue_capacity = 16 * 1024 * 1024;
    int log_flush_interval_ms = 100;
    int metrics_interval_ms = 0;

    TimeoutConfig timeouts;
    std::size_t max_request_body_size = 100 * 1024 * 1024;
    std::size_t max_response_body_size = 100 * 1024 * 1024;
    std::size_t worker_threads = 16;
    std::size_t max_connections = 64;
};

struct ParseResult {
    bool ok = false;
    bool help_requested = false;
    bool version_requested = false;
    std::string error;
    AppConfig config;
};

ParseResult parse_args(int argc, char** argv);
void print_help(std::ostream& os);
void print_version(std::ostream& os);
void print_config_summary(std::ostream& os, const AppConfig& config);

} // namespace ccs
