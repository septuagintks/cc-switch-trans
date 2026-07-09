#include "config.hpp"

#include <charconv>
#include <stdexcept>
#include <thread>
#include <unordered_set>

namespace ccs {

namespace {

constexpr const char* kVersion = "0.1.0";

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
    const auto* begin = value.data();
    const auto* end = value.data() + value.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) {
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
        "--responses-path",
        "--chat-path",
        "--usage-path",
        "--upstream-responses-path",
        "--upstream-chat-path",
        "--upstream-usage-path",
        "--log-path",
        "--log-level",
        "--log-body",
        "--redact-sensitive",
        "--body-log-limit",
        "--timeout-ms",
        "--max-body-size",
        "--concurrency",
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

bool validate_config(const AppConfig& config, std::string& error) {
    if (config.upstream_url.empty()) {
        error = "--upstream-url is required";
        return false;
    }
    if (config.listen_port == 0) {
        error = "--listen-port must be between 1 and 65535";
        return false;
    }
    if (config.timeout_ms <= 0) {
        error = "--timeout-ms must be greater than 0";
        return false;
    }
    if (config.max_body_size == 0) {
        error = "--max-body-size must be greater than 0";
        return false;
    }
    if (config.body_log_limit == 0) {
        error = "--body-log-limit must be greater than 0";
        return false;
    }
    if (config.concurrency == 0) {
        error = "--concurrency must be greater than 0";
        return false;
    }
    if (!is_valid_log_level(config.log_level)) {
        error = "--log-level must be one of trace, debug, info, warn, error";
        return false;
    }
    if (config.responses_path.empty() || config.responses_path.front() != '/') {
        error = "--responses-path must start with /";
        return false;
    }
    if (config.chat_path.empty() || config.chat_path.front() != '/') {
        error = "--chat-path must start with /";
        return false;
    }
    if (config.upstream_responses_path.empty() || config.upstream_responses_path.front() != '/') {
        error = "--upstream-responses-path must start with /";
        return false;
    }
    if (config.upstream_chat_path.empty() || config.upstream_chat_path.front() != '/') {
        error = "--upstream-chat-path must start with /";
        return false;
    }
    if (config.usage_path.empty() || config.usage_path.front() != '/') {
        error = "--usage-path must start with /";
        return false;
    }
    if (config.upstream_usage_path.empty() || config.upstream_usage_path.front() != '/') {
        error = "--upstream-usage-path must start with /";
        return false;
    }

    return true;
}

} // namespace

ParseResult parse_args(int argc, char** argv) {
    ParseResult result;
    result.config.concurrency = std::thread::hardware_concurrency();
    if (result.config.concurrency == 0) {
        result.config.concurrency = 4;
    }

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
                result.config.upstream_url = value;
            } else if (arg == "--responses-path") {
                result.config.responses_path = value;
            } else if (arg == "--chat-path") {
                result.config.chat_path = value;
            } else if (arg == "--usage-path") {
                result.config.usage_path = value;
            } else if (arg == "--upstream-responses-path") {
                result.config.upstream_responses_path = value;
            } else if (arg == "--upstream-chat-path") {
                result.config.upstream_chat_path = value;
            } else if (arg == "--upstream-usage-path") {
                result.config.upstream_usage_path = value;
            } else if (arg == "--log-path") {
                result.config.log_path = value;
            } else if (arg == "--log-level") {
                result.config.log_level = value;
            } else if (arg == "--log-body") {
                bool parsed = false;
                if (!parse_bool(value, parsed)) {
                    result.error = "--log-body must be true or false";
                    return result;
                }
                result.config.log_body = parsed;
            } else if (arg == "--redact-sensitive") {
                bool parsed = false;
                if (!parse_bool(value, parsed)) {
                    result.error = "--redact-sensitive must be true or false";
                    return result;
                }
                result.config.redact_sensitive = parsed;
            } else if (arg == "--body-log-limit") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--body-log-limit must be a positive integer";
                    return result;
                }
                result.config.body_log_limit = parsed;
            } else if (arg == "--timeout-ms") {
                int parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--timeout-ms must be a positive integer";
                    return result;
                }
                result.config.timeout_ms = parsed;
            } else if (arg == "--max-body-size") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--max-body-size must be a positive integer";
                    return result;
                }
                result.config.max_body_size = parsed;
            } else if (arg == "--concurrency") {
                std::size_t parsed = 0;
                if (!parse_integer(value, parsed)) {
                    result.error = "--concurrency must be a positive integer";
                    return result;
                }
                result.config.concurrency = parsed;
            }
        }
    } catch (const std::exception& ex) {
        result.error = ex.what();
        return result;
    }

    if (!validate_config(result.config, result.error)) {
        return result;
    }

    result.ok = true;
    return result;
}

void print_help(std::ostream& os) {
    os
        << "ccs-trans " << kVersion << "\n"
        << "\n"
        << "Usage:\n"
        << "  ccs-trans --upstream-url <url> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --listen-host <host>              Local listen host (default: 127.0.0.1)\n"
        << "  --listen-port <port>              Local listen port (default: 15723)\n"
        << "  --upstream-url <url>              Upstream base URL (required)\n"
        << "  --responses-path <path>           Local Responses path (default: /v1/responses/)\n"
        << "  --chat-path <path>                Local Chat Completions path (default: /v1/chat/completions)\n"
        << "  --usage-path <path>               Local Usage path (default: /v1/usage)\n"
        << "  --upstream-responses-path <path>  Upstream Responses path (default: /v1/responses/)\n"
        << "  --upstream-chat-path <path>       Upstream Chat Completions path (default: /v1/chat/completions)\n"
        << "  --upstream-usage-path <path>      Upstream Usage path (default: /v1/usage)\n"
        << "  --log-path <path>                 Log file path (default: ./logs/ccs-trans.log)\n"
        << "  --log-level <level>               trace, debug, info, warn, error (default: info)\n"
        << "  --log-body <true|false>           Write request/response bodies (default: true)\n"
        << "  --redact-sensitive <true|false>   Redact sensitive headers (default: false)\n"
        << "  --body-log-limit <bytes>          Per-body log limit (default: 1048576)\n"
        << "  --timeout-ms <ms>                 Upstream timeout (default: 300000)\n"
        << "  --max-body-size <bytes>           Max local request body size (default: 104857600)\n"
        << "  --concurrency <count>             Server worker count (default: CPU cores)\n"
        << "  --help, -h                        Show this help message\n"
        << "  --version                         Show version information\n";
}

void print_version(std::ostream& os) {
    os << "ccs-trans " << kVersion << "\n";
}

void print_config_summary(std::ostream& os, const AppConfig& config) {
    os
        << "ccs-trans configuration\n"
        << "  listen: " << config.listen_host << ":" << config.listen_port << "\n"
        << "  upstream_url: " << config.upstream_url << "\n"
        << "  responses_path: " << config.responses_path << "\n"
        << "  chat_path: " << config.chat_path << "\n"
        << "  usage_path: " << config.usage_path << "\n"
        << "  upstream_responses_path: " << config.upstream_responses_path << "\n"
        << "  upstream_chat_path: " << config.upstream_chat_path << "\n"
        << "  upstream_usage_path: " << config.upstream_usage_path << "\n"
        << "  log_path: " << config.log_path.string() << "\n"
        << "  log_level: " << config.log_level << "\n"
        << "  log_body: " << (config.log_body ? "true" : "false") << "\n"
        << "  redact_sensitive: " << (config.redact_sensitive ? "true" : "false") << "\n"
        << "  body_log_limit: " << config.body_log_limit << "\n"
        << "  timeout_ms: " << config.timeout_ms << "\n"
        << "  max_body_size: " << config.max_body_size << "\n"
        << "  concurrency: " << config.concurrency << "\n";
}

} // namespace ccs
