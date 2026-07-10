#pragma once

#include "core/task.hpp"
#include "core/timeouts.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace ccs {

struct AppConfig {
    EndpointGroupConfig responses_endpoint{
        EndpointGroupKind::Responses,
        "127.0.0.1",
        15723,
        "",
        {
            ApiTaskKind::Responses,
            "POST",
            "/v1/responses/",
            "/v1/responses/",
            {"remove_findcg_image_gen"},
            true,
        },
        {
            ApiTaskKind::ResponsesUsage,
            "GET",
            "/v1/usage",
            "/v1/usage",
            {},
            false,
        },
    };
    EndpointGroupConfig chat_endpoint{
        EndpointGroupKind::Chat,
        "127.0.0.1",
        15724,
        "",
        {
            ApiTaskKind::ChatCompletions,
            "POST",
            "/v1/chat/completions",
            "/v1/chat/completions",
            {},
            true,
        },
        {
            ApiTaskKind::ChatUsage,
            "GET",
            "/v1/usage",
            "/v1/usage",
            {},
            false,
        },
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
    std::size_t worker_threads = 32;
    std::size_t max_connections = 64;
};

using ConfigSnapshot = std::shared_ptr<const AppConfig>;

enum class CliCommandKind {
    Run,
    ProfileList,
    ProfileShow,
    ProfileCreate,
    ProfileRemove,
    ProfileUse,
    ProfileSet,
    ProfileUnset,
    Help,
    Version,
};

enum class ConfigValueType {
    String,
    Boolean,
    Integer,
};

struct ConfigOverride {
    std::string key;
    std::string value;
};

struct ParseResult {
    bool ok = false;
    bool help_requested = false;
    bool version_requested = false;
    std::string error;
    CliCommandKind command = CliCommandKind::Run;
    std::string profile_name;
    std::string profile_key;
    std::string profile_value;
    std::vector<ConfigOverride> overrides;
    AppConfig config;
};

ParseResult parse_args(int argc, char** argv);
bool validate_config(const AppConfig& config, std::string& error);
bool validate_profile_config(const AppConfig& config, std::string& error);
bool apply_config_override(
    AppConfig& config,
    const std::string& key,
    const std::string& value,
    std::string& error);
std::optional<ConfigValueType> config_value_type(const std::string& key);
bool is_valid_profile_name(const std::string& name);
ConfigSnapshot make_config_snapshot(AppConfig config);
void print_help(std::ostream& os);
void print_version(std::ostream& os);
void print_config_summary(std::ostream& os, const AppConfig& config);

} // namespace ccs
