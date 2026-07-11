#include "config/app_paths.hpp"
#include "config/config.hpp"
#include "config/config_document.hpp"
#include "config/profile_store.hpp"
#include "config/runtime_compiler.hpp"
#include "core/app_service.hpp"
#include "core/cancellation.hpp"
#include "core/task_router.hpp"
#include "core/runtime_metrics.hpp"
#include "core/url.hpp"
#include "logging/logger.hpp"
#include "server/server.hpp"
#include "transforms/findcg_responses_transform.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ccs::ParseResult parse(std::initializer_list<const char*> args) {
    std::vector<std::string> values;
    values.emplace_back("ccs-trans");
    for (const auto* arg : args) {
        values.emplace_back(arg);
    }
    std::vector<char*> pointers;
    pointers.reserve(values.size());
    for (auto& value : values) {
        pointers.push_back(value.data());
    }
    return ccs::parse_args(static_cast<int>(pointers.size()), pointers.data());
}

ccs::ConfigDocument runtime_document(
    std::uint16_t port,
    const std::filesystem::path& log_path,
    std::string upstream = "https://example.com",
    std::string host = "127.0.0.1") {
    auto document = ccs::make_default_config_document();
    document.application.listener.host = std::move(host);
    document.application.listener.port = port;
    document.application.logging.path = log_path.generic_string();
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = "/v1/responses";
    profile.upstream.base_url = std::move(upstream);
    profile.upstream.request_path = "/v1/responses";
    document.profiles.emplace("primary", std::move(profile));
    return document;
}

ccs::RuntimeSnapshotPtr compile_runtime(const ccs::ConfigDocument& document) {
    ccs::RuntimeCompiler compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-core-runtime");
    ccs::RuntimeSnapshotPtr snapshot;
    std::string error;
    require(compiler.compile(document, {}, snapshot, error), error);
    return snapshot;
}

void test_config_resolution() {
    const auto defaults = parse({"run", "--responses-upstream-url", "https://example.com"});
    require(defaults.ok, defaults.error);
    require(defaults.config.worker_threads == 32, "default worker threads");
    require(defaults.config.max_connections == 64, "default max connections");
    require(defaults.config.responses_endpoint.listen_port == 15723, "default Responses port");
    require(defaults.config.chat_endpoint.listen_port == 15724, "default Chat port");
    require(defaults.config.responses_endpoint.enabled(), "Responses endpoint enabled");
    require(!defaults.config.chat_endpoint.enabled(), "Chat endpoint disabled without its URL");
    std::string validation_error;
    require(ccs::validate_config(defaults.config, validation_error), validation_error);

    const auto canonical = parse({
        "run",
        "--responses-listen-host", "127.0.0.2",
        "--responses-listen-port", "16001",
        "--responses-upstream-url", "https://www.findcg.com",
        "--responses-local-path", "/local/responses",
        "--responses-upstream-path", "/upstream/responses",
        "--responses-usage-local-path", "/local/responses/usage",
        "--responses-usage-upstream-path", "/upstream/responses/usage",
        "--chat-listen-host", "127.0.0.3",
        "--chat-listen-port", "16002",
        "--chat-upstream-url", "https://chat.example.com",
        "--chat-local-path", "/local/chat",
        "--chat-upstream-path", "/upstream/chat",
        "--chat-usage-local-path", "/local/chat/usage",
        "--chat-usage-upstream-path", "/upstream/chat/usage",
        "--log-path", "custom.log",
        "--log-level", "debug",
        "--log-body", "false",
        "--redact-sensitive", "true",
        "--body-log-limit", "2048",
        "--worker-threads", "4",
        "--max-connections", "50",
        "--max-request-body-size", "8192",
        "--max-response-body-size", "16384",
        "--metrics-interval-ms", "250",
        "--log-queue-capacity", "4096",
        "--log-flush-interval-ms", "75",
        "--resolve-timeout-ms", "101",
        "--connect-timeout-ms", "102",
        "--send-timeout-ms", "103",
        "--response-header-timeout-ms", "104",
        "--stream-idle-timeout-ms", "105",
        "--total-timeout-ms", "106",
    });
    require(canonical.ok, canonical.error);
    require(canonical.config.responses_endpoint.listen_host == "127.0.0.2", "Responses host option");
    require(canonical.config.responses_endpoint.listen_port == 16001, "Responses port option");
    require(canonical.config.responses_endpoint.main_task.local_path == "/local/responses", "Responses local path");
    require(canonical.config.responses_endpoint.main_task.upstream_path == "/upstream/responses", "Responses upstream path");
    require(canonical.config.responses_endpoint.usage_task.local_path == "/local/responses/usage", "Responses Usage local path");
    require(canonical.config.responses_endpoint.usage_task.upstream_path == "/upstream/responses/usage", "Responses Usage upstream path");
    require(canonical.config.chat_endpoint.listen_host == "127.0.0.3", "Chat host option");
    require(canonical.config.chat_endpoint.listen_port == 16002, "Chat port option");
    require(canonical.config.chat_endpoint.main_task.local_path == "/local/chat", "Chat local path");
    require(canonical.config.chat_endpoint.main_task.upstream_path == "/upstream/chat", "Chat upstream path");
    require(canonical.config.chat_endpoint.usage_task.local_path == "/local/chat/usage", "Chat Usage local path");
    require(canonical.config.chat_endpoint.usage_task.upstream_path == "/upstream/chat/usage", "Chat Usage upstream path");
    require(canonical.config.log_path == "custom.log", "log path option");
    require(canonical.config.log_level == "debug", "log level option");
    require(!canonical.config.log_body, "log body option");
    require(canonical.config.redact_sensitive, "redaction option");
    require(canonical.config.body_log_limit == 2048, "body log limit option");
    require(canonical.config.worker_threads == 4, "worker thread option");
    require(canonical.config.max_connections == 50, "max connection option");
    require(canonical.config.max_request_body_size == 8192, "request body limit option");
    require(canonical.config.max_response_body_size == 16384, "response body limit option");
    require(canonical.config.metrics_interval_ms == 250, "metrics interval option");
    require(canonical.config.log_queue_capacity == 4096, "log queue capacity option");
    require(canonical.config.log_flush_interval_ms == 75, "log flush interval option");
    require(canonical.config.timeouts.resolve_ms == 101, "resolve timeout option");
    require(canonical.config.timeouts.connect_ms == 102, "connect timeout option");
    require(canonical.config.timeouts.send_ms == 103, "send timeout option");
    require(canonical.config.timeouts.response_header_ms == 104, "response header timeout option");
    require(canonical.config.timeouts.stream_idle_ms == 105, "stream idle timeout option");
    require(canonical.config.timeouts.total_ms == 106, "total timeout option");
    require(ccs::validate_config(canonical.config, validation_error), validation_error);

    const std::vector<std::string> removed_options = {
        "--upstream-url",
        "--listen-host",
        "--listen-port",
        "--responses-path",
        "--chat-path",
        "--usage-path",
        "--upstream-responses-path",
        "--upstream-chat-path",
        "--upstream-usage-path",
        "--timeout-ms",
        "--max-body-size",
        "--concurrency",
        "-h",
    };
    for (const auto& option : removed_options) {
        const auto removed = parse({"run", option.c_str(), "1"});
        require(!removed.ok && removed.error.find("removed option") != std::string::npos,
            option + " has an explicit migration error");
    }

    const auto missing_command = parse({});
    require(!missing_command.ok && missing_command.error.find("missing command") != std::string::npos,
        "run command is required");
    const auto old_invocation = parse({"--responses-upstream-url", "https://example.com"});
    require(!old_invocation.ok && old_invocation.error.find("unknown command") != std::string::npos,
        "options cannot replace the run command");
    const auto duplicate = parse({
        "run",
        "--responses-upstream-url", "https://example.com",
        "--responses-upstream-url", "https://other.example.com",
    });
    require(!duplicate.ok && duplicate.error.find("duplicate option") != std::string::npos,
        "duplicate options are rejected");
    const auto bool_alias = parse({
        "run", "--responses-upstream-url", "https://example.com", "--log-body", "yes",
    });
    require(!bool_alias.ok && bool_alias.error.find("true or false") != std::string::npos,
        "boolean aliases are rejected");
    const auto same_listener = parse({
        "run",
        "--responses-upstream-url", "https://example.com",
        "--chat-upstream-url", "https://chat.example.com",
        "--chat-listen-port", "15723",
    });
    require(same_listener.ok, same_listener.error);
    require(!ccs::validate_config(same_listener.config, validation_error)
            && validation_error.find("same listen address") != std::string::npos,
        "enabled endpoints cannot share a listen address");
    const auto disabled_same_listener = parse({
        "run",
        "--responses-upstream-url", "https://example.com",
        "--chat-listen-port", "15723",
    });
    require(disabled_same_listener.ok, disabled_same_listener.error);
    require(!ccs::validate_config(disabled_same_listener.config, validation_error)
            && validation_error.find("same listen address") != std::string::npos,
        "all bound endpoints require distinct listen addresses");
    const auto overlapping_routes = parse({
        "run",
        "--responses-upstream-url", "https://example.com",
        "--responses-local-path", "/same/",
        "--responses-usage-local-path", "/same",
    });
    require(overlapping_routes.ok, overlapping_routes.error);
    require(!ccs::validate_config(overlapping_routes.config, validation_error)
            && validation_error.find("must be different") != std::string::npos,
        "same-endpoint routes cannot overlap after canonicalization");
    const auto query_in_path = parse({
        "run",
        "--responses-upstream-url", "https://example.com",
        "--responses-upstream-path", "/v1/responses?fixed=true",
    });
    require(query_in_path.ok, query_in_path.error);
    require(!ccs::validate_config(query_in_path.config, validation_error)
            && validation_error.find("query or fragment") != std::string::npos,
        "configured paths cannot embed queries");

    const auto selected_profile = parse({"run", "--profile", "desktop", "--worker-threads", "8"});
    require(selected_profile.ok && selected_profile.profile_name == "desktop", "run profile parsed");
    require(selected_profile.overrides.size() == 1
            && selected_profile.overrides[0].key == "worker-threads",
        "run override remains separate from profile selection");
    const auto profile_set = parse({"profile", "set", "desktop", "worker-threads", "8"});
    require(profile_set.ok && profile_set.command == ccs::CliCommandKind::ProfileSet,
        "profile set command parsed");
    const auto prefixed_profile_key = parse({"profile", "set", "desktop", "--worker-threads", "8"});
    require(!prefixed_profile_key.ok && prefixed_profile_key.error.find("without --") != std::string::npos,
        "profile keys reject CLI prefixes");
    const auto invalid_profile_name = parse({"profile", "create", "../escape"});
    require(!invalid_profile_name.ok, "invalid profile name rejected");

    std::ostringstream help;
    ccs::print_help(help);
    const auto help_text = help.str();
    require(help_text.find("ccs-trans run [--profile <name>] [options]") != std::string::npos, "help documents run command");
    require(help_text.find("ccs-trans profile set <name> <key> <value>") != std::string::npos,
        "help documents profile commands");
    require(help_text.find("--responses-usage-upstream-path") != std::string::npos, "help documents canonical Usage option");
    require(help_text.find("--upstream-url") == std::string::npos, "help omits removed shared upstream option");
    require(help_text.find(", -h") == std::string::npos, "help omits removed short alias");
}

void test_url_and_router() {
    const auto parsed = ccs::parse_http_url("HTTPS://WWW.FindCG.com.:443/base");
    require(parsed.secure, "HTTPS parsed");
    require(parsed.host == "www.findcg.com", "host normalized");
    require(parsed.port == 443, "port parsed");
    require(ccs::is_findcg_host(parsed.host), "findcg matched");
    require(!ccs::is_findcg_host("findcg.com.example.org"), "suffix host rejected");

    const auto config_result = parse({
        "run",
        "--responses-upstream-url", "https://responses.example.com",
        "--chat-upstream-url", "https://chat.example.com",
    });
    require(config_result.ok, config_result.error);
    ccs::TaskRouter responses_router({&config_result.config.responses_endpoint});
    ccs::TaskRouter chat_router({&config_result.config.chat_endpoint});
    require(responses_router.route("/v1/responses").task->kind == ccs::ApiTaskKind::Responses, "Responses no slash");
    require(responses_router.route("/v1/responses/").task->kind == ccs::ApiTaskKind::Responses, "Responses slash");
    const auto responses_usage = responses_router.route("/v1/usage");
    require(responses_usage.task->kind == ccs::ApiTaskKind::ResponsesUsage, "Responses Usage ownership");
    require(responses_usage.upstream_target().base_url == "https://responses.example.com", "Responses Usage upstream");
    const auto chat_usage = chat_router.route("/v1/usage");
    require(chat_usage.task->kind == ccs::ApiTaskKind::ChatUsage, "Chat Usage ownership");
    require(chat_usage.upstream_target().base_url == "https://chat.example.com", "Chat Usage upstream");
    require(responses_router.route("/v1/chat/completions").task == nullptr, "cross-endpoint route rejected");
    require(responses_router.route("/unknown").task == nullptr, "unknown route");
}

void test_upstream_proxy_policy() {
#ifdef _WIN32
    require(std::string(ccs::upstream_proxy_mode()) == "windows_system", "Windows uses system proxy mode");
#else
    require(std::string(ccs::upstream_proxy_mode()) == "unsupported", "non-Windows transport is not implemented");
#endif
}

std::string read_file(const std::filesystem::path& path);

std::filesystem::path fixture_path(const std::filesystem::path& relative) {
#ifndef CCS_TRANS_TEST_FIXTURE_DIR
#error "CCS_TRANS_TEST_FIXTURE_DIR must be defined for core tests"
#endif
    return std::filesystem::path(CCS_TRANS_TEST_FIXTURE_DIR) / relative;
}

void test_findcg_transform() {
    ccs::FindcgResponsesTransform transform;
    ccs::TaskConfig task{
        ccs::ApiTaskKind::Responses,
        "POST",
        "/v1/responses/",
        "/v1/responses/",
        {"remove_findcg_image_gen"},
        true,
    };

    const auto fixture = nlohmann::json::parse(read_file(
        fixture_path("stage11/findcg-transform-cases.json")));
    require(
        fixture["schema_version"] == "ccs-trans.fixture/findcg-transform/v1",
        "findcg fixture schema");
    require(fixture["cases"].is_array() && !fixture["cases"].empty(), "findcg fixture cases");

    for (const auto& test_case : fixture["cases"]) {
        const auto name = test_case.at("name").get<std::string>();
        const auto body = test_case.at("body").get<std::string>();
        ccs::UpstreamTarget upstream{
            test_case.at("upstream_url").get<std::string>(),
            "/v1/responses/",
        };
        const bool expects_error = test_case.contains("expected_error_status");
        try {
            const auto result = transform.apply(task, upstream, body);
            require(!expects_error, name + ": expected TransformError");
            const auto& expected = test_case.at("expected");
            require(result.matched == expected.at("matched").get<bool>(), name + ": matched");
            require(result.modified == expected.at("modified").get<bool>(), name + ": modified");
            require(
                result.rewrite_reason == expected.at("rewrite_reason").get<std::string>(),
                name + ": rewrite reason");
            require(
                result.removed_tools.size() == expected.at("removed_tools_count").get<std::size_t>(),
                name + ": removed tool count");
            require(result.original_body_size == body.size(), name + ": original body size");
            if (!result.modified) {
                require(!result.rewritten_body, name + ": transparent body has no replacement");
                require(result.rewritten_body_size == body.size(), name + ": transparent body size");
            } else {
                require(result.rewritten_body.has_value(), name + ": rewritten body exists");
                require(
                    result.rewritten_body_size == result.rewritten_body->size(),
                    name + ": rewritten body size");
                require(
                    nlohmann::json::parse(*result.rewritten_body) == expected.at("body"),
                    name + ": rewritten body semantics");
            }
        } catch (const ccs::TransformError& ex) {
            require(expects_error, name + ": unexpected TransformError");
            require(
                ex.status_code() == test_case.at("expected_error_status").get<int>(),
                name + ": TransformError status");
        }
    }
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
}

template <typename Predicate>
bool wait_until(std::chrono::milliseconds timeout, Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return predicate();
}

struct ControlledSinkState {
    std::mutex mutex;
    std::condition_variable cv;
    std::string data;
    std::size_t flush_count = 0;
    bool block_first_flush = false;
    bool first_flush_entered = false;
    bool release_first_flush = false;
    bool fail_flush = false;
};

class ControlledLogSink final : public ccs::LogSink {
public:
    explicit ControlledLogSink(std::shared_ptr<ControlledSinkState> state)
        : state_(std::move(state)) {}

    bool open(const std::filesystem::path&, std::string&) override {
        return true;
    }

    bool write(std::string_view data, std::string&) override {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->data.append(data);
        return true;
    }

    bool flush(std::string& error) override {
        std::unique_lock<std::mutex> lock(state_->mutex);
        ++state_->flush_count;
        if (state_->block_first_flush && state_->flush_count == 1) {
            state_->first_flush_entered = true;
            state_->cv.notify_all();
            state_->cv.wait(lock, [&]() { return state_->release_first_flush; });
        }
        if (state_->fail_flush) {
            error = "injected flush failure";
            return false;
        }
        return true;
    }

    void close() noexcept override {}

private:
    std::shared_ptr<ControlledSinkState> state_;
};

void test_logger_flush_contract() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
        / ("ccs-trans-core-logger-test-" + std::to_string(nonce) + ".log");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        ccs::LoggerConfig config;
        config.path = path;
        config.flush_interval_ms = 50;
        auto metrics = std::make_shared<ccs::RuntimeMetrics>();
        ccs::Logger logger(config, metrics);
        std::string error;
        require(logger.open(error), error);
        require(logger.log("info", "idle_flush", {}), "normal log is accepted");
        require(wait_until(std::chrono::milliseconds(500), [&]() {
            return read_file(path).find("idle_flush") != std::string::npos;
        }), "normal record is visible after the batch window without a later event");
        const auto idle_snapshot = metrics->snapshot();
        require(idle_snapshot.log_records_written == 1, "idle batch is recorded as written");
        require(idle_snapshot.log_backpressure_count == 0, "batch wait is not queue backpressure");
        require(idle_snapshot.max_log_batch_wait_us > 0, "batch window wait is measured");
        require(idle_snapshot.max_log_record_age_us > 0, "record age is measured");
        require(idle_snapshot.log_writer_healthy == 1, "writer health is visible");

        require(logger.log("info", "queued_before_error", {}), "record before error is accepted");
        require(logger.log("error", "flush_now", {}), "error record is durable on return");
        const auto immediate = read_file(path);
        require(immediate.find("queued_before_error") != std::string::npos, "error flushes queued records");
        require(immediate.find("flush_now") != std::string::npos, "error record is durable on return");
        require(logger.log("info", "drain_on_destroy", {}), "shutdown record is accepted");
        const auto snapshot = metrics->snapshot();
        require(snapshot.log_records_enqueued == 4, "logger enqueue metrics");
        require(snapshot.peak_log_queue_records >= 1, "logger queue high water metric");
        require(snapshot.log_file_write_time_us <= snapshot.log_write_time_us, "write duration is classified");
        require(snapshot.log_file_flush_time_us <= snapshot.log_write_time_us, "flush duration is classified");
        require(logger.status().state == ccs::LogWriterState::Running, "writer status is running");
    }

    const auto final = read_file(path);
    require(final.find("drain_on_destroy") != std::string::npos, "destructor drains normal records");
    std::filesystem::remove(path, ec);
}

void test_logger_backpressure_contract() {
    ccs::LoggerConfig config;
    config.flush_interval_ms = 1;
    config.queue_capacity = 256;
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    auto sink_state = std::make_shared<ControlledSinkState>();
    sink_state->block_first_flush = true;

    ccs::Logger logger(
        config,
        metrics,
        std::make_unique<ControlledLogSink>(sink_state));
    std::string error;
    require(logger.open(error), error);
    require(logger.log("info", "fills_capacity", {
        ccs::field_string("body", std::string(512, 'x')),
    }), "oversized first record is accepted as the sole pending record");

    {
        std::unique_lock<std::mutex> lock(sink_state->mutex);
        require(sink_state->cv.wait_for(lock, std::chrono::milliseconds(500), [&]() {
            return sink_state->first_flush_entered;
        }), "writer entered the injected slow flush");
    }

    std::atomic<bool> second_finished{false};
    std::atomic<bool> second_accepted{false};
    std::thread producer([&]() {
        second_accepted.store(
            logger.log("info", "waits_for_capacity", {}),
            std::memory_order_release);
        second_finished.store(true, std::memory_order_release);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(!second_finished.load(std::memory_order_acquire), "in-flight records retain queue capacity");
    require(metrics->snapshot().oldest_log_record_age_us > 0, "oldest pending age grows during slow flush");

    {
        std::lock_guard<std::mutex> lock(sink_state->mutex);
        sink_state->release_first_flush = true;
    }
    sink_state->cv.notify_all();
    producer.join();
    require(second_accepted.load(std::memory_order_acquire), "producer resumes after capacity is released");
    const auto snapshot = metrics->snapshot();
    require(snapshot.log_backpressure_count == 1, "capacity wait is classified as backpressure");
    require(snapshot.log_backpressure_wait_us > 0, "backpressure duration is measured");
}

void test_logger_failure_contract() {
    ccs::LoggerConfig config;
    config.flush_interval_ms = 1;
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    auto sink_state = std::make_shared<ControlledSinkState>();
    sink_state->fail_flush = true;
    std::mutex reported_failure_mutex;
    std::string reported_failure;

    ccs::Logger logger(
        config,
        metrics,
        std::make_unique<ControlledLogSink>(sink_state),
        [&](const std::string& callback_error) {
            std::lock_guard<std::mutex> lock(reported_failure_mutex);
            reported_failure = callback_error;
        });
    std::string error;
    require(logger.open(error), error);
    require(!logger.log("error", "must_report_failure", {}), "error log reports failed durability");
    const auto status = logger.status();
    require(status.state == ccs::LogWriterState::Failed, "writer enters failed state");
    require(status.error == "injected flush failure", "writer status keeps the sink error");
    require(status.pending_records == 1, "failed record remains visible as pending");
    {
        std::lock_guard<std::mutex> lock(reported_failure_mutex);
        require(reported_failure == status.error, "host failure callback receives the sink error");
    }
    require(!logger.log("info", "rejected_after_failure", {}), "logging after writer failure is rejected");
    const auto snapshot = metrics->snapshot();
    require(snapshot.log_writer_failures == 1, "writer failure metric increments");
    require(snapshot.log_writer_healthy == 0, "writer health metric clears on failure");
    require(snapshot.log_records_written == 0, "failed flush is not counted as durable output");
}

void test_logger_open_failure_contract() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto parent_file = std::filesystem::temp_directory_path()
        / ("ccs-trans-core-logger-parent-" + std::to_string(nonce));
    {
        std::ofstream output(parent_file, std::ios::binary);
        output << "not a directory";
    }

    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    {
        ccs::LoggerConfig config;
        config.path = parent_file / "ccs-trans.log";
        ccs::Logger logger(config, metrics);
        std::string error;
        require(!logger.open(error), "invalid log directory is rejected");
        require(error.find("failed to create log directory") != std::string::npos, "directory error is actionable");
        const auto status = logger.status();
        require(status.state == ccs::LogWriterState::Failed, "open failure enters failed state");
        require(status.error == error, "open failure remains queryable");
    }
    require(metrics->snapshot().log_writer_failures == 1, "open failure metric increments");
    std::error_code ec;
    std::filesystem::remove(parent_file, ec);
}

void test_profile_store() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-profile-store-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);

    ccs::ProfileStore store(paths);
    std::string error;
    require(store.load(error), error);
    require(store.profile_names().empty(), "missing config starts with no profiles");
    require(!store.create("../invalid", error), "store API rejects invalid profile names");
    require(store.create("desktop", error), error);
    require(!store.create("desktop", error), "duplicate profile rejected");
    require(store.set("desktop", "responses-upstream-url", "https://profile.example.com", error), error);
    require(store.set("desktop", "log-body", "false", error), error);
    require(store.set("desktop", "worker-threads", "16", error), error);
    require(store.set("desktop", "max-connections", "32", error), error);
    require(!store.set("desktop", "chat-listen-port", "15723", error),
        "profile set rejects cross-field listener conflicts");
    require(!store.set("desktop", "log-path", "../outside.log", error),
        "relative profile log path cannot escape the app root");
    require(!store.set("desktop", "log-path", ".", error),
        "application root cannot be used as a log file");
    require(store.create("other", error), error);
    require(store.set("other", "responses-upstream-url", "https://other.example.com", error), error);
    require(store.use("desktop", error), error);
    require(store.save(error), error);
    require(std::filesystem::exists(paths.config_file), "config file saved");
    require(std::filesystem::is_directory(paths.logs_directory), "logs directory created");
    require(std::filesystem::is_directory(paths.state_directory), "state directory created");

    const auto saved = nlohmann::json::parse(read_file(paths.config_file));
    require(saved["schema_version"] == "ccs-trans.config/v1", "config schema version");
    require(saved["active_profile"] == "desktop", "active profile saved");
    require(saved["profiles"]["desktop"]["log-body"].is_boolean(), "boolean profile value is typed");
    require(saved["profiles"]["desktop"]["worker-threads"].is_number_unsigned(),
        "integer profile value is typed");

    ccs::ProfileStore reloaded(paths);
    require(reloaded.load(error), error);
    require(reloaded.active_profile() && *reloaded.active_profile() == "desktop", "active profile loaded");
    const auto run_active = parse({"run"});
    require(run_active.ok, run_active.error);
    ccs::ConfigSnapshot active_snapshot;
    std::string selected;
    require(reloaded.resolve_run(run_active, active_snapshot, selected, error), error);
    require(selected == "desktop", "active profile selected by default");
    require(active_snapshot->responses_endpoint.upstream_url == "https://profile.example.com",
        "profile upstream applied");
    require(!active_snapshot->log_body, "profile boolean applied");
    require(active_snapshot->worker_threads == 16 && active_snapshot->max_connections == 32,
        "profile integer values applied");
    require(active_snapshot->log_path == paths.default_log_file, "default log path uses app root");

    const auto run_explicit = parse({"run", "--profile", "other"});
    require(run_explicit.ok, run_explicit.error);
    ccs::ConfigSnapshot explicit_snapshot;
    require(reloaded.resolve_run(run_explicit, explicit_snapshot, selected, error), error);
    require(selected == "other" && explicit_snapshot->responses_endpoint.upstream_url == "https://other.example.com",
        "explicit run profile overrides active profile selection");

    const auto run_override = parse({
        "run",
        "--profile", "desktop",
        "--responses-upstream-url", "https://override.example.com",
        "--worker-threads", "8",
        "--max-connections", "16",
        "--log-path", "logs/override.log",
    });
    require(run_override.ok, run_override.error);
    ccs::ConfigSnapshot override_snapshot;
    require(reloaded.resolve_run(run_override, override_snapshot, selected, error), error);
    require(override_snapshot->responses_endpoint.upstream_url == "https://override.example.com",
        "CLI upstream overrides profile");
    require(override_snapshot->worker_threads == 8 && override_snapshot->max_connections == 16,
        "CLI capacity overrides profile");
    require(override_snapshot->log_path == paths.root / "logs/override.log",
        "relative log path resolves under app root");

    const auto missing_profile = parse({"run", "--profile", "missing", "--responses-upstream-url", "https://example.com"});
    ccs::ConfigSnapshot missing_snapshot;
    require(!reloaded.resolve_run(missing_profile, missing_snapshot, selected, error)
            && error.find("does not exist") != std::string::npos,
        "explicit missing profile rejected");
    const auto escaping_log = parse({
        "run", "--profile", "desktop", "--log-path", "../outside.log",
    });
    require(!reloaded.resolve_run(escaping_log, missing_snapshot, selected, error)
            && error.find("application root") != std::string::npos,
        "relative CLI log path cannot escape the app root");

    std::string shown;
    require(reloaded.show_json("desktop", shown, error), error);
    const auto shown_json = nlohmann::json::parse(shown);
    require(shown_json["active"] == true && shown_json["values"]["worker-threads"] == 16,
        "profile show output is typed");
    require(reloaded.unset("desktop", "worker-threads", error), error);
    require(!reloaded.unset("desktop", "worker-threads", error), "unset missing profile key rejected");
    require(reloaded.remove("desktop", error), error);
    require(!reloaded.active_profile(), "removing active profile clears selection");
    require(reloaded.save(error), error);

    for (const auto& entry : std::filesystem::directory_iterator(paths.root)) {
        require(entry.path().filename().string().find("config.json.tmp-") == std::string::npos,
            "atomic save leaves no temporary file");
    }

    {
        std::ofstream corrupt(paths.config_file, std::ios::binary | std::ios::trunc);
        corrupt << R"({"schema_version":"unknown","active_profile":null,"profiles":{}})";
    }
    ccs::ProfileStore corrupt_store(paths);
    require(!corrupt_store.load(error) && error.find("schema_version") != std::string::npos,
        "unsupported schema is rejected without fallback");
    require(!corrupt_store.save(error) && error.find("loaded") != std::string::npos,
        "failed load cannot be saved over the damaged config");

    {
        std::ofstream wrong_type(paths.config_file, std::ios::binary | std::ios::trunc);
        wrong_type << R"({"schema_version":"ccs-trans.config/v1","active_profile":null,"profiles":{"bad":{"worker-threads":"32"}}})";
    }
    ccs::ProfileStore wrong_type_store(paths);
    require(!wrong_type_store.load(error) && error.find("JSON integer") != std::string::npos,
        "profile JSON types are enforced");

    {
        std::ofstream unknown_key(paths.config_file, std::ios::binary | std::ios::trunc);
        unknown_key << R"({"schema_version":"ccs-trans.config/v1","active_profile":null,"profiles":{"bad":{"authorization":"secret"}}})";
    }
    ccs::ProfileStore unknown_key_store(paths);
    require(!unknown_key_store.load(error) && error.find("unknown config key") != std::string::npos,
        "unknown and credential-like profile fields are rejected");

    std::filesystem::remove_all(root, ec);
}

void test_stage11_config_fixture() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto root = std::filesystem::temp_directory_path()
        / ("ccs-trans-stage11-config-fixture-" + std::to_string(nonce));
    const auto paths = ccs::make_app_paths(root);
    std::error_code ec;
    std::filesystem::remove_all(root, ec);
    std::filesystem::create_directories(root);
    std::filesystem::copy_file(
        fixture_path("stage11/config-v1-read-only.json"),
        paths.config_file,
        std::filesystem::copy_options::overwrite_existing);

    const auto before = read_file(paths.config_file);
    ccs::ProfileStore store(paths);
    std::string error;
    require(store.load(error), error);
    require(read_file(paths.config_file) == before, "loading baseline config does not rewrite fixture bytes");
    require(
        store.active_profile() && *store.active_profile() == "synthetic-baseline",
        "baseline fixture active profile");

    const auto run = parse({"run"});
    require(run.ok, run.error);
    ccs::ConfigSnapshot snapshot;
    std::string selected;
    require(store.resolve_run(run, snapshot, selected, error), error);
    require(selected == "synthetic-baseline", "baseline fixture profile selected");
    require(
        snapshot->responses_endpoint.upstream_url == "https://responses.baseline.invalid",
        "baseline fixture Responses upstream");
    require(
        snapshot->chat_endpoint.upstream_url == "https://chat.baseline.invalid",
        "baseline fixture Chat upstream");
    require(snapshot->worker_threads == 16 && snapshot->max_connections == 32,
        "baseline fixture capacity");
    require(!snapshot->log_body && snapshot->redact_sensitive,
        "baseline fixture logging policy");

    std::filesystem::remove_all(root, ec);
}

void test_runtime_metrics() {
    ccs::RuntimeMetrics metrics;
    metrics.connection_accepted(1, 1);
    metrics.connection_accepted(2, 2);
    metrics.worker_started(1, 25);
    metrics.request_started();
    metrics.stream_started();
    metrics.stream_chunk_forwarded(512);
    metrics.upstream_request_started();
    metrics.upstream_request_failed();
    metrics.upstream_timeout(ccs::UpstreamTimeoutPhase::ResponseHeader);
    metrics.upstream_timeout(ccs::UpstreamTimeoutPhase::StreamIdle);
    metrics.upstream_timeout(ccs::UpstreamTimeoutPhase::Total);
    metrics.request_completed();
    metrics.worker_finished(1);
    metrics.connection_rejected();

    const auto snapshot = metrics.snapshot();
    require(snapshot.connections_accepted == 2, "accepted connection metric");
    require(snapshot.connections_rejected == 1, "rejected connection metric");
    require(snapshot.peak_connections == 2, "connection high water metric");
    require(snapshot.peak_queued_connections == 2, "queue high water metric");
    require(snapshot.peak_active_workers == 1, "worker high water metric");
    require(snapshot.connection_queue_wait_time_us == 25, "global connection queue wait metric");
    require(snapshot.connections_completed == 1, "completed connection metric");
    require(snapshot.max_connection_queue_wait_us == 25, "maximum queue wait metric");
    require(snapshot.stream_chunks_forwarded == 1, "stream chunk metric");
    require(snapshot.stream_bytes_forwarded == 512, "stream byte metric");
    require(snapshot.upstream_requests_failed == 1, "upstream failure metric");
    require(snapshot.upstream_response_header_timeouts == 1, "response header timeout metric");
    require(snapshot.upstream_stream_idle_timeouts == 1, "stream idle timeout metric");
    require(snapshot.upstream_total_timeouts == 1, "total timeout metric");
}

void test_app_service_startup_failure() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto log_path = std::filesystem::temp_directory_path()
        / ("ccs-trans-service-startup-" + std::to_string(nonce) + ".log");
    {
        const auto snapshot = compile_runtime(runtime_document(
            15723, log_path, "https://example.com", "256.256.256.256"));
        ccs::AppService service(snapshot);
        std::string error;
        require(!service.start(error), "service startup failure is synchronous");
        require(error.find("failed to resolve listen address") != std::string::npos,
            "service startup returns the listener error");
        require(service.status() == ccs::ServiceState::Stopped, "failed service remains stopped");
        require(service.wait() != 0, "failed service keeps a non-zero exit code");
        std::string retry_error;
        require(!service.start(retry_error), "failed service can retry startup after joining its thread");
        require(retry_error.find("failed to resolve listen address") != std::string::npos,
            "retried startup returns the listener error");
        require(service.wait() != 0, "retried startup failure remains non-zero");
    }
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
}

void test_server_reload_classification() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto log_path = std::filesystem::temp_directory_path()
        / ("ccs-trans-server-reload-" + std::to_string(nonce) + ".log");
    auto document = runtime_document(15723, log_path, "https://responses-a.example.com");
    ccs::Server server(compile_runtime(document));

    std::string error;
    auto hot_document = document;
    hot_document.profiles.at("primary").upstream.base_url = "https://responses-b.example.com";
    require(
        server.reload(compile_runtime(hot_document), error) == ccs::ReloadResult::Applied,
        "upstream-only reload is applied in place: " + error);

    auto restart_document = hot_document;
    ++restart_document.application.runtime.worker_threads;
    error.clear();
    require(
        server.reload(compile_runtime(restart_document), error)
            == ccs::ReloadResult::RestartRequired,
        "worker topology reload requires restart");

    error.clear();
    require(
        server.reload({}, error) == ccs::ReloadResult::Failed && !error.empty(),
        "null reload snapshot is rejected");

    std::error_code ec;
    std::filesystem::remove(log_path, ec);
}

void test_app_service_reload_and_rollback() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto responses_port = static_cast<std::uint16_t>(30000 + nonce % 10000);
    const auto log_path = std::filesystem::temp_directory_path()
        / ("ccs-trans-service-reload-" + std::to_string(nonce) + ".log");
    auto document = runtime_document(
        responses_port, log_path, "https://responses-a.example.com");

    ccs::AppService service(compile_runtime(document));
    std::string error;
    require(service.start(error), "reload test service starts: " + error);

    auto hot_document = document;
    hot_document.profiles.at("primary").upstream.base_url = "https://responses-b.example.com";
    require(
        service.reload(compile_runtime(hot_document), error),
        "service applies hot reload: " + error);
    require(service.status() == ccs::ServiceState::Running, "service remains running after hot reload");

    auto restart_document = hot_document;
    ++restart_document.application.runtime.worker_threads;
    error.clear();
    require(
        service.reload(compile_runtime(restart_document), error),
        "service performs graceful restart reload: " + error);
    require(service.status() == ccs::ServiceState::Running, "service runs after restart reload");

    auto invalid_topology = restart_document;
    invalid_topology.application.listener.host = "256.256.256.256";
    error.clear();
    require(
        !service.reload(compile_runtime(invalid_topology), error),
        "failed restart reload reports failure");
    require(
        error.find("previous configuration was restored") != std::string::npos,
        "failed restart reload reports rollback");
    require(service.status() == ccs::ServiceState::Running, "rollback restores running service");

    int wait_result = -1;
    std::thread waiter([&]() { wait_result = service.wait(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    service.stop();
    waiter.join();
    require(wait_result == 0, "reloaded service stops cleanly while another thread waits");
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
}

void test_server_v2_route_and_protocol_errors() {
    const auto log_path = std::filesystem::temp_directory_path()
        / "ccs-trans-v2-route-errors.log";
    auto document = runtime_document(15723, log_path);
    ccs::RuleDefinition responses_rule;
    responses_rule.id.value = "remove-image";
    responses_rule.enabled = true;
    responses_rule.type = "remove_tool";
    responses_rule.options["tool"] = "image_gen";
    document.profiles.at("primary").rules.push_back(responses_rule);

    ccs::ProfileDefinition messages;
    messages.enabled = true;
    messages.protocol = ccs::ProtocolId{"messages"};
    messages.local.request_path = "/v1/messages";
    messages.upstream.base_url = "https://messages.example.com";
    messages.upstream.request_path = "/v1/messages";
    auto messages_rule = responses_rule;
    messages_rule.id.value = "remove-messages-image";
    messages.rules.push_back(std::move(messages_rule));
    document.profiles.emplace("messages", std::move(messages));

    ccs::Server server(compile_runtime(document));
    const auto send = [&](const std::string& method, const std::string& path, const std::string& body) {
        return server.process_raw_request(
            method + " " + path + " HTTP/1.1\r\nContent-Type: application/json\r\n"
                "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body,
            "127.0.0.1");
    };
    const auto response_body = [](const std::string& response) {
        const auto separator = response.find("\r\n\r\n");
        require(separator != std::string::npos, "local response has an HTTP header");
        return nlohmann::json::parse(response.substr(separator + 4));
    };

    const auto invalid_json = send("POST", "/v1/responses", "not json");
    require(invalid_json.find("HTTP/1.1 400") == 0, "Responses rule parse error returns 400");
    require(response_body(invalid_json)["error"]["type"] == "invalid_request_error",
        "Responses rule error uses the OpenAI envelope");

    const auto messages_root = send("POST", "/v1/messages", "[]");
    require(messages_root.find("HTTP/1.1 400") == 0, "Messages rule shape error returns 400");
    const auto messages_error = response_body(messages_root);
    require(messages_error["type"] == "error"
            && messages_error["error"]["type"] == "invalid_request_error",
        "Messages rule error uses the Anthropic envelope");

    const auto wrong_method = send("GET", "/v1/responses", "");
    require(wrong_method.find("HTTP/1.1 405") == 0
            && wrong_method.find("Allow: POST") != std::string::npos,
        "known v2 path returns 405 with Allow");
    require(send("POST", "/unknown", "{}").find("HTTP/1.1 404") == 0,
        "unknown v2 path returns 404");
    require(send("POST", "/v1//responses", "{}").find("HTTP/1.1 400") == 0,
        "non-canonical request path returns 400");
}

void test_cancellation() {
    ccs::CancellationSource source;
    const auto token = source.token();
    int callback_count = 0;
    {
        auto registration = token.on_cancel([&callback_count]() {
            ++callback_count;
        });
        require(!token.is_cancelled(), "token starts active");
        require(source.cancel(), "first cancellation wins");
        require(token.is_cancelled(), "token observes cancellation");
        require(callback_count == 1, "registered callback invoked once");
        require(!source.cancel(), "second cancellation is idempotent");
    }

    int immediate_count = 0;
    auto immediate = token.on_cancel([&immediate_count]() {
        ++immediate_count;
    });
    require(immediate_count == 1, "late callback invoked immediately");

    ccs::CancellationSource inactive_source;
    int inactive_count = 0;
    {
        auto inactive = inactive_source.token().on_cancel([&inactive_count]() {
            ++inactive_count;
        });
    }
    inactive_source.cancel();
    require(inactive_count == 0, "destroyed registration is inactive");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"config resolution", test_config_resolution},
        {"URL and router", test_url_and_router},
        {"upstream proxy policy", test_upstream_proxy_policy},
        {"findcg transform", test_findcg_transform},
        {"logger flush contract", test_logger_flush_contract},
        {"logger backpressure contract", test_logger_backpressure_contract},
        {"logger failure contract", test_logger_failure_contract},
        {"logger open failure contract", test_logger_open_failure_contract},
        {"profile store", test_profile_store},
        {"stage 11 config fixture", test_stage11_config_fixture},
        {"runtime metrics", test_runtime_metrics},
        {"app service startup failure", test_app_service_startup_failure},
        {"server reload classification", test_server_reload_classification},
        {"app service reload and rollback", test_app_service_reload_and_rollback},
        {"server v2 route and protocol errors", test_server_v2_route_and_protocol_errors},
        {"cancellation", test_cancellation},
    };

    for (const auto& [name, test] : tests) {
        try {
            test();
        } catch (const std::exception& ex) {
            std::cerr << name << " failed: " << ex.what() << "\n";
            return 1;
        }
    }
    std::cout << "core tests ok\n";
    return 0;
}
