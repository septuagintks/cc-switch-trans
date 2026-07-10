#include "config/config.hpp"

#include "core/url.hpp"

#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace ccs {

namespace {

constexpr const char* kVersion = "0.3.0";

struct Overrides {
    std::optional<std::string> shared_upstream_url;
    std::optional<std::string> responses_upstream_url;
    std::optional<std::string> chat_upstream_url;
    std::optional<std::string> legacy_responses_path;
    std::optional<std::string> legacy_chat_path;
    std::optional<std::string> responses_path;
    std::optional<std::string> chat_path;
    std::optional<std::size_t> legacy_max_body_size;
    std::optional<std::size_t> max_request_body_size;
    std::optional<std::size_t> max_response_body_size;
    std::optional<std::size_t> worker_threads;
    std::optional<std::size_t> max_connections;
    std::optional<int> legacy_timeout_ms;
    std::optional<int> resolve_timeout_ms;
    std::optional<int> connect_timeout_ms;
    std::optional<int> send_timeout_ms;
    std::optional<int> response_header_timeout_ms;
    std::optional<int> stream_idle_timeout_ms;
    std::optional<int> total_timeout_ms;
};

bool parse_bool(const std::string& value, bool& out) {
    if (value == "true" || value == "1" || value == "yes" || value == "on") {
        out = true;
        return true;
    }
    if (value == "false" || value == "0" || value == "no" || value == "off") {
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

bool needs_value(const std::string& arg) {
    static const std::unordered_set<std::string> keys = {
        "--listen-host",
        "--listen-port",
        "--upstream-url",
        "--responses-upstream-url",
        "--chat-upstream-url",
        "--responses-path",
        "--chat-path",
        "--usage-path",
        "--upstream-responses-path",
        "--upstream-chat-path",
        "--responses-upstream-path",
        "--chat-upstream-path",
        "--upstream-usage-path",
        "--log-path",
        "--log-level",
        "--log-body",
        "--redact-sensitive",
        "--body-log-limit",
        "--metrics-interval-ms",
        "--timeout-ms",
        "--resolve-timeout-ms",
        "--connect-timeout-ms",
        "--send-timeout-ms",
        "--response-header-timeout-ms",
        "--stream-idle-timeout-ms",
        "--total-timeout-ms",
        "--max-body-size",
        "--max-request-body-size",
        "--max-response-body-size",
        "--worker-threads",
        "--max-connections",
    };
    return keys.count(arg) != 0;
}

std::string take_value(int& i, int argc, char** argv, const std::string& key) {
    if (i + 1 >= argc) {
        throw std::runtime_error(key + " requires a value");
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
    return true;
}

bool validate_task(const TaskConfig& task, const std::string& label, std::string& error) {
    if (!validate_path(task.local_path, label + " local path", error)
        || !validate_path(task.upstream.path, label + " upstream path", error)) {
        return false;
    }
    if (!task.enabled) {
        return true;
    }
    try {
        (void)parse_http_url(task.upstream.base_url);
    } catch (const std::exception& ex) {
        error = label + " upstream URL: " + ex.what();
        return false;
    }
    return true;
}

bool validate_config(const AppConfig& config, std::string& error) {
    if (!config.responses.enabled && !config.chat_completions.enabled) {
        error = "set --upstream-url or a Responses/Chat upstream URL";
        return false;
    }
    if (config.listen_port == 0) {
        error = "--listen-port must be between 1 and 65535";
        return false;
    }
    if (config.timeouts.resolve_ms <= 0
        || config.timeouts.connect_ms <= 0
        || config.timeouts.send_ms <= 0
        || config.timeouts.response_header_ms <= 0
        || config.timeouts.stream_idle_ms <= 0
        || config.timeouts.total_ms < 0) {
        error = "stage timeouts must be greater than 0; --total-timeout-ms may also be 0";
        return false;
    }
    if (config.max_request_body_size == 0 || config.max_response_body_size == 0) {
        error = "request and response body limits must be greater than 0";
        return false;
    }
    if (config.body_log_limit == 0) {
        error = "--body-log-limit must be greater than 0";
        return false;
    }
    if (config.worker_threads == 0) {
        error = "--worker-threads must be greater than 0";
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
    return validate_task(config.responses, "Responses", error)
        && validate_task(config.chat_completions, "Chat Completions", error)
        && validate_task(config.usage, "Usage", error);
}

void resolve_overrides(AppConfig& config, const Overrides& overrides) {
    const std::string shared = overrides.shared_upstream_url.value_or("");
    config.responses.upstream.base_url = overrides.responses_upstream_url.value_or(shared);
    config.chat_completions.upstream.base_url = overrides.chat_upstream_url.value_or(shared);
    config.usage.upstream.base_url = shared;
    config.responses.enabled = !config.responses.upstream.base_url.empty();
    config.chat_completions.enabled = !config.chat_completions.upstream.base_url.empty();
    config.usage.enabled = !config.usage.upstream.base_url.empty();

    if (overrides.legacy_responses_path) {
        config.responses.upstream.path = *overrides.legacy_responses_path;
    }
    if (overrides.legacy_chat_path) {
        config.chat_completions.upstream.path = *overrides.legacy_chat_path;
    }
    if (overrides.responses_path) {
        config.responses.upstream.path = *overrides.responses_path;
    }
    if (overrides.chat_path) {
        config.chat_completions.upstream.path = *overrides.chat_path;
    }

    config.max_request_body_size = overrides.max_request_body_size.value_or(
        overrides.legacy_max_body_size.value_or(config.max_request_body_size));
    config.max_response_body_size = overrides.max_response_body_size.value_or(config.max_response_body_size);
    config.worker_threads = overrides.worker_threads.value_or(config.worker_threads);
    config.max_connections = overrides.max_connections.value_or(
        std::max<std::size_t>(64, config.worker_threads * 4));

    const int fallback_timeout = overrides.legacy_timeout_ms.value_or(300000);
    config.timeouts.resolve_ms = overrides.resolve_timeout_ms.value_or(fallback_timeout);
    config.timeouts.connect_ms = overrides.connect_timeout_ms.value_or(fallback_timeout);
    config.timeouts.send_ms = overrides.send_timeout_ms.value_or(fallback_timeout);
    config.timeouts.response_header_ms = overrides.response_header_timeout_ms.value_or(fallback_timeout);
    config.timeouts.stream_idle_ms = overrides.stream_idle_timeout_ms.value_or(fallback_timeout);
    config.timeouts.total_ms = overrides.total_timeout_ms.value_or(0);
}

} // namespace

ParseResult parse_args(int argc, char** argv) {
    ParseResult result;
    Overrides overrides;
    const auto hardware_threads = std::thread::hardware_concurrency();
    result.config.worker_threads = hardware_threads == 0
        ? 8
        : std::clamp<std::size_t>(hardware_threads, 8, 16);
    result.config.max_connections = std::max<std::size_t>(64, result.config.worker_threads * 4);

    try {
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            if (arg == "--help" || arg == "-h") {
                result.ok = true;
                result.help_requested = true;
                return result;
            }
            if (arg == "--version") {
                result.ok = true;
                result.version_requested = true;
                return result;
            }
            if (!needs_value(arg)) {
                result.error = "unknown option: " + arg;
                return result;
            }

            const std::string value = take_value(i, argc, argv, arg);
            if (arg == "--listen-host") {
                result.config.listen_host = value;
            } else if (arg == "--listen-port") {
                unsigned int port = 0;
                if (!parse_integer(value, port) || port == 0 || port > 65535) {
                    result.error = "--listen-port must be between 1 and 65535";
                    return result;
                }
                result.config.listen_port = static_cast<std::uint16_t>(port);
            } else if (arg == "--upstream-url") {
                overrides.shared_upstream_url = value;
            } else if (arg == "--responses-upstream-url") {
                overrides.responses_upstream_url = value;
            } else if (arg == "--chat-upstream-url") {
                overrides.chat_upstream_url = value;
            } else if (arg == "--responses-path") {
                result.config.responses.local_path = value;
            } else if (arg == "--chat-path") {
                result.config.chat_completions.local_path = value;
            } else if (arg == "--usage-path") {
                result.config.usage.local_path = value;
            } else if (arg == "--upstream-responses-path") {
                overrides.legacy_responses_path = value;
            } else if (arg == "--upstream-chat-path") {
                overrides.legacy_chat_path = value;
            } else if (arg == "--responses-upstream-path") {
                overrides.responses_path = value;
            } else if (arg == "--chat-upstream-path") {
                overrides.chat_path = value;
            } else if (arg == "--upstream-usage-path") {
                result.config.usage.upstream.path = value;
            } else if (arg == "--log-path") {
                result.config.log_path = value;
            } else if (arg == "--log-level") {
                result.config.log_level = value;
            } else if (arg == "--log-body") {
                if (!parse_bool(value, result.config.log_body)) {
                    result.error = "--log-body must be true or false";
                    return result;
                }
            } else if (arg == "--redact-sensitive") {
                if (!parse_bool(value, result.config.redact_sensitive)) {
                    result.error = "--redact-sensitive must be true or false";
                    return result;
                }
            } else if (arg == "--body-log-limit") {
                if (!parse_integer(value, result.config.body_log_limit)) {
                    result.error = "--body-log-limit must be a positive integer";
                    return result;
                }
            } else if (arg == "--metrics-interval-ms") {
                if (!parse_integer(value, result.config.metrics_interval_ms)) {
                    result.error = "--metrics-interval-ms must be a non-negative integer";
                    return result;
                }
            } else if (arg == "--timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed) || parsed <= 0) {
                    result.error = "--timeout-ms must be a positive integer";
                    return result;
                }
                overrides.legacy_timeout_ms = parsed;
            } else if (arg == "--resolve-timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed) || parsed <= 0) {
                    result.error = "--resolve-timeout-ms must be a positive integer";
                    return result;
                }
                overrides.resolve_timeout_ms = parsed;
            } else if (arg == "--connect-timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed) || parsed <= 0) {
                    result.error = "--connect-timeout-ms must be a positive integer";
                    return result;
                }
                overrides.connect_timeout_ms = parsed;
            } else if (arg == "--send-timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed) || parsed <= 0) {
                    result.error = "--send-timeout-ms must be a positive integer";
                    return result;
                }
                overrides.send_timeout_ms = parsed;
            } else if (arg == "--response-header-timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed) || parsed <= 0) {
                    result.error = "--response-header-timeout-ms must be a positive integer";
                    return result;
                }
                overrides.response_header_timeout_ms = parsed;
            } else if (arg == "--stream-idle-timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed) || parsed <= 0) {
                    result.error = "--stream-idle-timeout-ms must be a positive integer";
                    return result;
                }
                overrides.stream_idle_timeout_ms = parsed;
            } else if (arg == "--total-timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--total-timeout-ms must be a non-negative integer";
                    return result;
                }
                overrides.total_timeout_ms = parsed;
            } else if (arg == "--max-body-size") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--max-body-size must be a positive integer";
                    return result;
                }
                overrides.legacy_max_body_size = parsed;
            } else if (arg == "--max-request-body-size") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--max-request-body-size must be a positive integer";
                    return result;
                }
                overrides.max_request_body_size = parsed;
            } else if (arg == "--max-response-body-size") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--max-response-body-size must be a positive integer";
                    return result;
                }
                overrides.max_response_body_size = parsed;
            } else if (arg == "--worker-threads") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--worker-threads must be a positive integer";
                    return result;
                }
                overrides.worker_threads = parsed;
            } else if (arg == "--max-connections") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--max-connections must be a positive integer";
                    return result;
                }
                overrides.max_connections = parsed;
            }
        }
    } catch (const std::exception& ex) {
        result.error = ex.what();
        return result;
    }

    resolve_overrides(result.config, overrides);
    if (!validate_config(result.config, result.error)) {
        return result;
    }
    result.ok = true;
    return result;
}

void print_help(std::ostream& os) {
    os
        << "ccs-trans " << kVersion << "\n\n"
        << "Usage:\n"
        << "  ccs-trans [--upstream-url <url>] [task upstream options] [options]\n\n"
        << "Options:\n"
        << "  --listen-host <host>                Local listen host (default: 127.0.0.1)\n"
        << "  --listen-port <port>                Local listen port (default: 15723)\n"
        << "  --upstream-url <url>                Shared legacy upstream and Usage upstream\n"
        << "  --responses-upstream-url <url>      Responses upstream URL\n"
        << "  --chat-upstream-url <url>           Chat Completions upstream URL\n"
        << "  --responses-path <path>             Local Responses path (default: /v1/responses/)\n"
        << "  --chat-path <path>                  Local Chat path (default: /v1/chat/completions)\n"
        << "  --usage-path <path>                 Local Usage path (default: /v1/usage)\n"
        << "  --responses-upstream-path <path>    Responses upstream path\n"
        << "  --chat-upstream-path <path>         Chat upstream path\n"
        << "  --upstream-responses-path <path>    Legacy Responses upstream path\n"
        << "  --upstream-chat-path <path>         Legacy Chat upstream path\n"
        << "  --upstream-usage-path <path>        Usage upstream path\n"
        << "  --log-path <path>                   Log file path (default: ./logs/ccs-trans.log)\n"
        << "  --log-level <level>                 trace, debug, info, warn, error\n"
        << "  --log-body <true|false>             Write request/response bodies\n"
        << "  --redact-sensitive <true|false>     Redact sensitive headers\n"
        << "  --body-log-limit <bytes>            Per-event body log limit\n"
        << "  --metrics-interval-ms <ms>          Performance snapshot interval (0 disables)\n"
        << "  --timeout-ms <ms>                   Upstream timeout (default: 300000)\n"
        << "  --resolve-timeout-ms <ms>           DNS resolution timeout\n"
        << "  --connect-timeout-ms <ms>           Upstream connection timeout\n"
        << "  --send-timeout-ms <ms>              Upstream request send timeout\n"
        << "  --response-header-timeout-ms <ms>   Upstream response header timeout\n"
        << "  --stream-idle-timeout-ms <ms>       Maximum idle gap between SSE data\n"
        << "  --total-timeout-ms <ms>             Optional whole-request timeout (0 disables)\n"
        << "  --max-request-body-size <bytes>     Max local request body size\n"
        << "  --max-response-body-size <bytes>    Max buffered upstream response size\n"
        << "  --max-body-size <bytes>             Legacy request body size option\n"
        << "  --worker-threads <count>            Blocking server worker count\n"
        << "  --max-connections <count>           Active and queued connection limit\n"
        << "  --help, -h                          Show this help message\n"
        << "  --version                           Show version information\n";
}

void print_version(std::ostream& os) {
    os << "ccs-trans " << kVersion << "\n";
}

void print_config_summary(std::ostream& os, const AppConfig& config) {
    const auto print_task = [&](const TaskConfig& task) {
        os << "  " << task_name(task.kind) << ": " << (task.enabled ? "enabled" : "disabled");
        if (task.enabled) {
            os << " -> " << task.upstream.base_url << task.upstream.path;
        }
        os << "\n";
    };

    os << "ccs-trans configuration\n"
       << "  listen: " << config.listen_host << ":" << config.listen_port << "\n";
    print_task(config.responses);
    print_task(config.chat_completions);
    print_task(config.usage);
    os << "  log_path: " << config.log_path.string() << "\n"
       << "  log_level: " << config.log_level << "\n"
       << "  log_body: " << (config.log_body ? "true" : "false") << "\n"
       << "  redact_sensitive: " << (config.redact_sensitive ? "true" : "false") << "\n"
       << "  body_log_limit: " << config.body_log_limit << "\n"
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
