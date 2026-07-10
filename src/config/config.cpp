#include "config/config.hpp"

#include "core/url.hpp"

#include <charconv>
#include <cctype>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ccs {

namespace {

constexpr const char* kVersion = "0.4.0";

const std::unordered_set<std::string>& value_options() {
    static const std::unordered_set<std::string> options = {
        "--responses-listen-host",
        "--responses-listen-port",
        "--responses-upstream-url",
        "--responses-local-path",
        "--responses-upstream-path",
        "--responses-usage-local-path",
        "--responses-usage-upstream-path",
        "--chat-listen-host",
        "--chat-listen-port",
        "--chat-upstream-url",
        "--chat-local-path",
        "--chat-upstream-path",
        "--chat-usage-local-path",
        "--chat-usage-upstream-path",
        "--log-path",
        "--log-level",
        "--log-body",
        "--redact-sensitive",
        "--body-log-limit",
        "--log-queue-capacity",
        "--log-flush-interval-ms",
        "--metrics-interval-ms",
        "--resolve-timeout-ms",
        "--connect-timeout-ms",
        "--send-timeout-ms",
        "--response-header-timeout-ms",
        "--stream-idle-timeout-ms",
        "--total-timeout-ms",
        "--max-request-body-size",
        "--max-response-body-size",
        "--worker-threads",
        "--max-connections",
    };
    return options;
}

const std::unordered_map<std::string, std::string>& removed_options() {
    static const std::unordered_map<std::string, std::string> options = {
        {"--upstream-url", "use --responses-upstream-url and/or --chat-upstream-url"},
        {"--listen-host", "use --responses-listen-host and --chat-listen-host"},
        {"--listen-port", "use --responses-listen-port and --chat-listen-port"},
        {"--responses-path", "use --responses-local-path"},
        {"--chat-path", "use --chat-local-path"},
        {"--usage-path", "use --responses-usage-local-path and --chat-usage-local-path"},
        {"--upstream-responses-path", "use --responses-upstream-path"},
        {"--upstream-chat-path", "use --chat-upstream-path"},
        {"--upstream-usage-path", "use the endpoint-specific Usage upstream path options"},
        {"--timeout-ms", "use the individual stage timeout options"},
        {"--max-body-size", "use --max-request-body-size"},
        {"--concurrency", "use --worker-threads and --max-connections"},
        {"-h", "use --help"},
    };
    return options;
}

bool removed_option_error(const std::string& option, std::string& error) {
    const auto it = removed_options().find(option);
    if (it == removed_options().end()) {
        return false;
    }
    error = "removed option " + option + ": " + it->second;
    return true;
}

bool parse_bool(const std::string& value, bool& out) {
    if (value == "true") {
        out = true;
        return true;
    }
    if (value == "false") {
        out = false;
        return true;
    }
    return false;
}

template <typename T>
bool parse_integer(const std::string& value, T& out) {
    if (value.empty() || value[0] == '-') {
        return false;
    }

    T parsed{};
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) {
        return false;
    }
    out = parsed;
    return true;
}

bool parse_port(const std::string& value, std::uint16_t& port, const std::string& option, std::string& error) {
    unsigned int parsed = 0;
    if (!parse_integer(value, parsed) || parsed == 0 || parsed > 65535) {
        error = option + " must be between 1 and 65535";
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

std::string take_value(int& i, int argc, char** argv, const std::string& option) {
    if (i + 1 >= argc) {
        throw std::runtime_error(option + " requires a value");
    }
    ++i;
    return argv[i];
}

bool is_valid_log_level(const std::string& level) {
    static const std::unordered_set<std::string> levels = {
        "trace", "debug", "info", "warn", "error",
    };
    return levels.count(level) != 0;
}

bool validate_path(const std::string& value, const std::string& option, std::string& error) {
    if (value.empty() || value.front() != '/') {
        error = option + " must start with /";
        return false;
    }
    if (value.find('?') != std::string::npos || value.find('#') != std::string::npos) {
        error = option + " must not contain a query or fragment";
        return false;
    }
    return true;
}

std::string canonical_route_path(std::string path) {
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

bool validate_endpoint(const EndpointGroupConfig& endpoint, const std::string& label, std::string& error) {
    if (endpoint.listen_host.empty()) {
        error = label + " listen host must not be empty";
        return false;
    }
    if (endpoint.listen_port == 0) {
        error = label + " listen port must be between 1 and 65535";
        return false;
    }
    if (!validate_path(endpoint.main_task.local_path, label + " local path", error)
        || !validate_path(endpoint.main_task.upstream_path, label + " upstream path", error)
        || !validate_path(endpoint.usage_task.local_path, label + " Usage local path", error)
        || !validate_path(endpoint.usage_task.upstream_path, label + " Usage upstream path", error)) {
        return false;
    }
    if (canonical_route_path(endpoint.main_task.local_path)
        == canonical_route_path(endpoint.usage_task.local_path)) {
        error = label + " main and Usage local paths must be different";
        return false;
    }
    if (!endpoint.enabled()) {
        return true;
    }
    try {
        (void)parse_http_url(endpoint.upstream_url);
    } catch (const std::exception& ex) {
        error = label + " upstream URL: " + ex.what();
        return false;
    }
    return true;
}

template <typename T>
bool assign_positive_integer(const std::string& value, T& target, const std::string& option, std::string& error) {
    T parsed{};
    if (!parse_integer(value, parsed) || parsed == 0) {
        error = option + " must be a positive integer";
        return false;
    }
    target = parsed;
    return true;
}

bool assign_non_negative_integer(
    const std::string& value,
    int& target,
    const std::string& option,
    std::string& error) {
    int parsed = 0;
    if (!parse_integer(value, parsed)) {
        error = option + " must be a non-negative integer";
        return false;
    }
    target = parsed;
    return true;
}

bool apply_option(AppConfig& config, const std::string& option, const std::string& value, std::string& error) {
    if (option == "--responses-listen-host") {
        config.responses_endpoint.listen_host = value;
    } else if (option == "--responses-listen-port") {
        return parse_port(value, config.responses_endpoint.listen_port, option, error);
    } else if (option == "--responses-upstream-url") {
        config.responses_endpoint.upstream_url = value;
    } else if (option == "--responses-local-path") {
        config.responses_endpoint.main_task.local_path = value;
    } else if (option == "--responses-upstream-path") {
        config.responses_endpoint.main_task.upstream_path = value;
    } else if (option == "--responses-usage-local-path") {
        config.responses_endpoint.usage_task.local_path = value;
    } else if (option == "--responses-usage-upstream-path") {
        config.responses_endpoint.usage_task.upstream_path = value;
    } else if (option == "--chat-listen-host") {
        config.chat_endpoint.listen_host = value;
    } else if (option == "--chat-listen-port") {
        return parse_port(value, config.chat_endpoint.listen_port, option, error);
    } else if (option == "--chat-upstream-url") {
        config.chat_endpoint.upstream_url = value;
    } else if (option == "--chat-local-path") {
        config.chat_endpoint.main_task.local_path = value;
    } else if (option == "--chat-upstream-path") {
        config.chat_endpoint.main_task.upstream_path = value;
    } else if (option == "--chat-usage-local-path") {
        config.chat_endpoint.usage_task.local_path = value;
    } else if (option == "--chat-usage-upstream-path") {
        config.chat_endpoint.usage_task.upstream_path = value;
    } else if (option == "--log-path") {
        config.log_path = value;
    } else if (option == "--log-level") {
        config.log_level = value;
    } else if (option == "--log-body") {
        if (!parse_bool(value, config.log_body)) {
            error = option + " must be true or false";
            return false;
        }
    } else if (option == "--redact-sensitive") {
        if (!parse_bool(value, config.redact_sensitive)) {
            error = option + " must be true or false";
            return false;
        }
    } else if (option == "--body-log-limit") {
        return assign_positive_integer(value, config.body_log_limit, option, error);
    } else if (option == "--log-queue-capacity") {
        return assign_positive_integer(value, config.log_queue_capacity, option, error);
    } else if (option == "--log-flush-interval-ms") {
        return assign_positive_integer(value, config.log_flush_interval_ms, option, error);
    } else if (option == "--metrics-interval-ms") {
        return assign_non_negative_integer(value, config.metrics_interval_ms, option, error);
    } else if (option == "--resolve-timeout-ms") {
        return assign_positive_integer(value, config.timeouts.resolve_ms, option, error);
    } else if (option == "--connect-timeout-ms") {
        return assign_positive_integer(value, config.timeouts.connect_ms, option, error);
    } else if (option == "--send-timeout-ms") {
        return assign_positive_integer(value, config.timeouts.send_ms, option, error);
    } else if (option == "--response-header-timeout-ms") {
        return assign_positive_integer(value, config.timeouts.response_header_ms, option, error);
    } else if (option == "--stream-idle-timeout-ms") {
        return assign_positive_integer(value, config.timeouts.stream_idle_ms, option, error);
    } else if (option == "--total-timeout-ms") {
        return assign_non_negative_integer(value, config.timeouts.total_ms, option, error);
    } else if (option == "--max-request-body-size") {
        return assign_positive_integer(value, config.max_request_body_size, option, error);
    } else if (option == "--max-response-body-size") {
        return assign_positive_integer(value, config.max_response_body_size, option, error);
    } else if (option == "--worker-threads") {
        return assign_positive_integer(value, config.worker_threads, option, error);
    } else if (option == "--max-connections") {
        return assign_positive_integer(value, config.max_connections, option, error);
    } else {
        error = "unknown option: " + option;
        return false;
    }
    return true;
}

bool validate_config_impl(const AppConfig& config, bool require_upstream, std::string& error) {
    if (require_upstream && !config.responses_endpoint.enabled() && !config.chat_endpoint.enabled()) {
        error = "set --responses-upstream-url and/or --chat-upstream-url";
        return false;
    }
    if (!validate_endpoint(config.responses_endpoint, "Responses endpoint", error)
        || !validate_endpoint(config.chat_endpoint, "Chat endpoint", error)) {
        return false;
    }
    if (config.responses_endpoint.listen_host == config.chat_endpoint.listen_host
        && config.responses_endpoint.listen_port == config.chat_endpoint.listen_port) {
        error = "Responses and Chat endpoints must not use the same listen address";
        return false;
    }
    if (config.max_connections < config.worker_threads) {
        error = "--max-connections must be greater than or equal to --worker-threads";
        return false;
    }
    if (!is_valid_log_level(config.log_level)) {
        error = "--log-level must be one of trace, debug, info, warn, error";
        return false;
    }
    const auto log_filename = config.log_path.filename();
    if (config.log_path.empty()
        || log_filename.empty()
        || log_filename == "."
        || log_filename == "..") {
        error = "--log-path must name a log file";
        return false;
    }
    if (config.body_log_limit == 0
        || config.log_queue_capacity == 0
        || config.log_flush_interval_ms <= 0
        || config.max_request_body_size == 0
        || config.max_response_body_size == 0
        || config.worker_threads == 0) {
        error = "size, worker, and log flush fields must be greater than 0";
        return false;
    }
    if (config.metrics_interval_ms < 0
        || config.timeouts.resolve_ms <= 0
        || config.timeouts.connect_ms <= 0
        || config.timeouts.send_ms <= 0
        || config.timeouts.response_header_ms <= 0
        || config.timeouts.stream_idle_ms <= 0
        || config.timeouts.total_ms < 0) {
        error = "metrics interval and total timeout may be 0; stage timeouts must be greater than 0";
        return false;
    }
    return true;
}

} // namespace

bool validate_config(const AppConfig& config, std::string& error) {
    return validate_config_impl(config, true, error);
}

bool validate_profile_config(const AppConfig& config, std::string& error) {
    return validate_config_impl(config, false, error);
}

bool apply_config_override(
    AppConfig& config,
    const std::string& key,
    const std::string& value,
    std::string& error) {
    if (key.empty() || key.rfind("--", 0) == 0) {
        error = "profile key must use the canonical name without --";
        return false;
    }
    const std::string option = "--" + key;
    if (value_options().count(option) == 0) {
        error = "unknown config key: " + key;
        return false;
    }
    return apply_option(config, option, value, error);
}

std::optional<ConfigValueType> config_value_type(const std::string& key) {
    const std::string option = "--" + key;
    if (value_options().count(option) == 0) {
        return std::nullopt;
    }
    if (option == "--log-body" || option == "--redact-sensitive") {
        return ConfigValueType::Boolean;
    }
    static const std::unordered_set<std::string> string_options = {
        "--responses-listen-host",
        "--responses-upstream-url",
        "--responses-local-path",
        "--responses-upstream-path",
        "--responses-usage-local-path",
        "--responses-usage-upstream-path",
        "--chat-listen-host",
        "--chat-upstream-url",
        "--chat-local-path",
        "--chat-upstream-path",
        "--chat-usage-local-path",
        "--chat-usage-upstream-path",
        "--log-path",
        "--log-level",
    };
    return string_options.count(option) != 0 ? ConfigValueType::String : ConfigValueType::Integer;
}

bool is_valid_profile_name(const std::string& name) {
    if (name.empty() || name.size() > 64 || !std::isalnum(static_cast<unsigned char>(name.front()))) {
        return false;
    }
    for (const unsigned char ch : name) {
        if (!std::isalnum(ch) && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

ConfigSnapshot make_config_snapshot(AppConfig config) {
    return std::make_shared<const AppConfig>(std::move(config));
}

ParseResult parse_args(int argc, char** argv) {
    ParseResult result;
    if (argc < 2) {
        result.error = "missing command: expected run";
        return result;
    }

    const std::string command = argv[1];
    if (command == "--help") {
        result.ok = true;
        result.help_requested = true;
        result.command = CliCommandKind::Help;
        return result;
    }
    if (command == "--version") {
        result.ok = true;
        result.version_requested = true;
        result.command = CliCommandKind::Version;
        return result;
    }
    if (removed_option_error(command, result.error)) {
        return result;
    }
    if (command == "profile") {
        if (argc < 3) {
            result.error = "profile requires a subcommand";
            return result;
        }
        const std::string subcommand = argv[2];
        const auto parse_profile_name = [&]() {
            if (argc < 4) {
                result.error = "profile " + subcommand + " requires a profile name";
                return false;
            }
            result.profile_name = argv[3];
            if (!is_valid_profile_name(result.profile_name)) {
                result.error = "profile name must be 1-64 characters using letters, digits, ., _, or -";
                return false;
            }
            return true;
        };

        if (subcommand == "list" && argc == 3) {
            result.command = CliCommandKind::ProfileList;
        } else if (subcommand == "show" && argc == 4 && parse_profile_name()) {
            result.command = CliCommandKind::ProfileShow;
        } else if (subcommand == "create" && argc == 4 && parse_profile_name()) {
            result.command = CliCommandKind::ProfileCreate;
        } else if (subcommand == "remove" && argc == 4 && parse_profile_name()) {
            result.command = CliCommandKind::ProfileRemove;
        } else if (subcommand == "use" && argc == 4 && parse_profile_name()) {
            result.command = CliCommandKind::ProfileUse;
        } else if (subcommand == "set" && argc == 6 && parse_profile_name()) {
            result.command = CliCommandKind::ProfileSet;
            result.profile_key = argv[4];
            result.profile_value = argv[5];
            std::string field_error;
            AppConfig probe;
            if (!apply_config_override(probe, result.profile_key, result.profile_value, field_error)) {
                result.error = field_error;
                return result;
            }
        } else if (subcommand == "unset" && argc == 5 && parse_profile_name()) {
            result.command = CliCommandKind::ProfileUnset;
            result.profile_key = argv[4];
            if (!config_value_type(result.profile_key)) {
                result.error = "unknown config key: " + result.profile_key;
                return result;
            }
        } else {
            if (result.error.empty()) {
                result.error = "invalid profile command or argument count";
            }
            return result;
        }
        result.ok = true;
        return result;
    }
    if (command != "run") {
        result.error = "unknown command: " + command;
        return result;
    }

    result.command = CliCommandKind::Run;
    std::unordered_set<std::string> seen;
    try {
        for (int i = 2; i < argc; ++i) {
            const std::string option = argv[i];
            if (option == "--help") {
                result.ok = true;
                result.help_requested = true;
                result.command = CliCommandKind::Help;
                return result;
            }
            if (option == "--version") {
                result.ok = true;
                result.version_requested = true;
                result.command = CliCommandKind::Version;
                return result;
            }
            if (removed_option_error(option, result.error)) {
                return result;
            }
            if (option == "--profile") {
                if (!seen.insert(option).second) {
                    result.error = "duplicate option: " + option;
                    return result;
                }
                result.profile_name = take_value(i, argc, argv, option);
                if (!is_valid_profile_name(result.profile_name)) {
                    result.error = "profile name must be 1-64 characters using letters, digits, ., _, or -";
                    return result;
                }
                continue;
            }
            if (value_options().count(option) == 0) {
                result.error = "unknown option: " + option;
                return result;
            }
            if (!seen.insert(option).second) {
                result.error = "duplicate option: " + option;
                return result;
            }
            const auto value = take_value(i, argc, argv, option);
            if (!apply_option(result.config, option, value, result.error)) {
                return result;
            }
            result.overrides.push_back(ConfigOverride{option.substr(2), value});
        }
    } catch (const std::exception& ex) {
        result.error = ex.what();
        return result;
    }

    result.ok = true;
    return result;
}

void print_help(std::ostream& os) {
    os
        << "ccs-trans " << kVersion << "\n\n"
        << "Usage:\n"
        << "  ccs-trans run [--profile <name>] [options]\n"
        << "  ccs-trans profile list\n"
        << "  ccs-trans profile show <name>\n"
        << "  ccs-trans profile create <name>\n"
        << "  ccs-trans profile remove <name>\n"
        << "  ccs-trans profile use <name>\n"
        << "  ccs-trans profile set <name> <key> <value>\n"
        << "  ccs-trans profile unset <name> <key>\n"
        << "  ccs-trans --help\n"
        << "  ccs-trans --version\n\n"
        << "Endpoint options:\n"
        << "  --responses-listen-host <host>          Responses listen host (default: 127.0.0.1)\n"
        << "  --responses-listen-port <port>          Responses listen port (default: 15723)\n"
        << "  --responses-upstream-url <url>          Responses endpoint upstream URL\n"
        << "  --responses-local-path <path>           Responses local route\n"
        << "  --responses-upstream-path <path>        Responses upstream route\n"
        << "  --responses-usage-local-path <path>     Responses Usage local route\n"
        << "  --responses-usage-upstream-path <path>  Responses Usage upstream route\n"
        << "  --chat-listen-host <host>               Chat listen host (default: 127.0.0.1)\n"
        << "  --chat-listen-port <port>               Chat listen port (default: 15724)\n"
        << "  --chat-upstream-url <url>               Chat endpoint upstream URL\n"
        << "  --chat-local-path <path>                Chat Completions local route\n"
        << "  --chat-upstream-path <path>             Chat Completions upstream route\n"
        << "  --chat-usage-local-path <path>          Chat Usage local route\n"
        << "  --chat-usage-upstream-path <path>       Chat Usage upstream route\n\n"
        << "Runtime options:\n"
        << "  --profile <name>                      Select a persistent profile for this run\n"
        << "  --log-path <path>                       Log file path\n"
        << "  --log-level <level>                     trace, debug, info, warn, error\n"
        << "  --log-body <true|false>                 Write request/response bodies\n"
        << "  --redact-sensitive <true|false>         Redact sensitive headers\n"
        << "  --body-log-limit <bytes>                Per-event body log limit\n"
        << "  --log-queue-capacity <bytes>            Pending log byte capacity\n"
        << "  --log-flush-interval-ms <ms>            Normal log batch window\n"
        << "  --metrics-interval-ms <ms>              Performance snapshot interval (0 disables)\n"
        << "  --resolve-timeout-ms <ms>               DNS resolution timeout\n"
        << "  --connect-timeout-ms <ms>               Upstream connection timeout\n"
        << "  --send-timeout-ms <ms>                  Upstream request send timeout\n"
        << "  --response-header-timeout-ms <ms>       Upstream response header timeout\n"
        << "  --stream-idle-timeout-ms <ms>           Maximum idle gap between SSE data\n"
        << "  --total-timeout-ms <ms>                 Whole-request timeout (0 disables)\n"
        << "  --max-request-body-size <bytes>         Max local request body size\n"
        << "  --max-response-body-size <bytes>        Max buffered upstream response size\n"
        << "  --worker-threads <count>                Maximum blocking server worker count\n"
        << "  --max-connections <count>               Active and queued connection limit\n"
        << "  --help                                  Show this help message\n"
        << "  --version                               Show version information\n";
}

void print_version(std::ostream& os) {
    os << "ccs-trans " << kVersion << "\n";
}

void print_config_summary(std::ostream& os, const AppConfig& config) {
    const auto print_endpoint = [&](const EndpointGroupConfig& endpoint) {
        os << "  " << endpoint_group_name(endpoint.kind) << "_endpoint: "
           << (endpoint.enabled() ? "enabled" : "disabled")
           << " listen=" << endpoint.listen_host << ":" << endpoint.listen_port;
        if (endpoint.enabled()) {
            os << " upstream=" << endpoint.upstream_url;
        }
        os << "\n"
           << "    " << task_name(endpoint.main_task.kind) << ": "
           << endpoint.main_task.local_path << " -> " << endpoint.main_task.upstream_path << "\n"
           << "    " << task_name(endpoint.usage_task.kind) << ": "
           << endpoint.usage_task.local_path << " -> " << endpoint.usage_task.upstream_path << "\n";
    };

    os << "ccs-trans configuration\n";
    print_endpoint(config.responses_endpoint);
    print_endpoint(config.chat_endpoint);
    os << "  log_path: " << config.log_path.string() << "\n"
       << "  log_level: " << config.log_level << "\n"
       << "  log_body: " << (config.log_body ? "true" : "false") << "\n"
       << "  redact_sensitive: " << (config.redact_sensitive ? "true" : "false") << "\n"
       << "  body_log_limit: " << config.body_log_limit << "\n"
       << "  log_queue_capacity: " << config.log_queue_capacity << "\n"
       << "  log_flush_interval_ms: " << config.log_flush_interval_ms << "\n"
       << "  metrics_interval_ms: " << config.metrics_interval_ms << "\n"
       << "  resolve_timeout_ms: " << config.timeouts.resolve_ms << "\n"
       << "  connect_timeout_ms: " << config.timeouts.connect_ms << "\n"
       << "  send_timeout_ms: " << config.timeouts.send_ms << "\n"
       << "  response_header_timeout_ms: " << config.timeouts.response_header_ms << "\n"
       << "  stream_idle_timeout_ms: " << config.timeouts.stream_idle_ms << "\n"
       << "  total_timeout_ms: " << config.timeouts.total_ms << "\n"
       << "  max_request_body_size: " << config.max_request_body_size << "\n"
       << "  max_response_body_size: " << config.max_response_body_size << "\n"
       << "  worker_threads: " << config.worker_threads << "\n"
       << "  max_connections: " << config.max_connections << "\n";
}

} // namespace ccs
