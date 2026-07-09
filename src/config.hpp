#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>

namespace ccs {

struct AppConfig {
    std::string listen_host = "127.0.0.1";
    std::uint16_t listen_port = 15723;
    std::string upstream_url;
    std::string responses_path = "/v1/responses/";
    std::string chat_path = "/v1/chat/completions";
    std::string upstream_responses_path = "/v1/responses/";
    std::string upstream_chat_path = "/v1/chat/completions";
    std::filesystem::path log_path = "./logs/ccs-trans.log";
    std::string log_level = "info";
    bool log_body = true;
    bool redact_sensitive = false;
    std::size_t body_log_limit = 1024 * 1024;
    int timeout_ms = 300000;
    std::size_t max_body_size = 100 * 1024 * 1024;
    std::size_t concurrency = 0;
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
