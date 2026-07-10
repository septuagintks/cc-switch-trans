#include "config/config.hpp"
#include "core/cancellation.hpp"
#include "core/task_router.hpp"
#include "core/runtime_metrics.hpp"
#include "core/url.hpp"
#include "logging/logger.hpp"
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

void test_config_resolution() {
    const auto defaults = parse({"--upstream-url", "https://example.com"});
    require(defaults.ok, defaults.error);
    require(defaults.config.worker_threads == 32, "default worker threads");
    require(defaults.config.max_connections == 64, "default max connections");

    const auto legacy = parse({"--upstream-url", "https://example.com", "--worker-threads", "8", "--max-connections", "64"});
    require(legacy.ok, legacy.error);
    require(legacy.config.responses.enabled, "legacy Responses enabled");
    require(legacy.config.chat_completions.enabled, "legacy Chat enabled");
    require(legacy.config.usage.enabled, "legacy Usage enabled");
    require(legacy.config.responses.upstream.base_url == "https://example.com", "legacy Responses URL");

    const auto split = parse({
        "--responses-upstream-url", "https://www.findcg.com",
        "--chat-upstream-url", "https://chat.example.com",
        "--upstream-responses-path", "/legacy",
        "--responses-upstream-path", "/v2/responses",
        "--worker-threads", "4",
        "--max-connections", "50",
        "--metrics-interval-ms", "250",
        "--resolve-timeout-ms", "101",
        "--connect-timeout-ms", "102",
        "--send-timeout-ms", "103",
        "--response-header-timeout-ms", "104",
        "--stream-idle-timeout-ms", "105",
        "--total-timeout-ms", "106",
    });
    require(split.ok, split.error);
    require(!split.config.usage.enabled, "Usage disabled without shared URL");
    require(split.config.responses.upstream.path == "/v2/responses", "canonical path wins");
    require(split.config.worker_threads == 4, "worker thread option");
    require(split.config.max_connections == 50, "max connection option");
    require(split.config.metrics_interval_ms == 250, "metrics interval option");
    require(split.config.timeouts.resolve_ms == 101, "resolve timeout option");
    require(split.config.timeouts.connect_ms == 102, "connect timeout option");
    require(split.config.timeouts.send_ms == 103, "send timeout option");
    require(split.config.timeouts.response_header_ms == 104, "response header timeout option");
    require(split.config.timeouts.stream_idle_ms == 105, "stream idle timeout option");
    require(split.config.timeouts.total_ms == 106, "total timeout option");

    const auto legacy_timeout = parse({"--upstream-url", "https://example.com", "--timeout-ms", "1234"});
    require(legacy_timeout.ok, legacy_timeout.error);
    require(legacy_timeout.config.timeouts.resolve_ms == 1234, "legacy resolve timeout fallback");
    require(legacy_timeout.config.timeouts.connect_ms == 1234, "legacy connect timeout fallback");
    require(legacy_timeout.config.timeouts.send_ms == 1234, "legacy send timeout fallback");
    require(legacy_timeout.config.timeouts.response_header_ms == 1234, "legacy header timeout fallback");
    require(legacy_timeout.config.timeouts.stream_idle_ms == 1234, "legacy stream timeout fallback");
    require(legacy_timeout.config.timeouts.total_ms == 0, "legacy timeout does not impose total deadline");

    const auto removed = parse({"--upstream-url", "https://example.com", "--concurrency", "4"});
    require(!removed.ok && removed.error.find("unknown option") != std::string::npos, "old concurrency rejected");
}

void test_url_and_router() {
    const auto parsed = ccs::parse_http_url("HTTPS://WWW.FindCG.com.:443/base");
    require(parsed.secure, "HTTPS parsed");
    require(parsed.host == "www.findcg.com", "host normalized");
    require(parsed.port == 443, "port parsed");
    require(ccs::is_findcg_host(parsed.host), "findcg matched");
    require(!ccs::is_findcg_host("findcg.com.example.org"), "suffix host rejected");

    const auto config_result = parse({"--upstream-url", "https://example.com"});
    require(config_result.ok, config_result.error);
    ccs::TaskRouter router(config_result.config);
    require(router.route("/v1/responses").task->kind == ccs::ApiTaskKind::Responses, "Responses no slash");
    require(router.route("/v1/responses/").task->kind == ccs::ApiTaskKind::Responses, "Responses slash");
    require(router.route("/unknown").task == nullptr, "unknown route");
}

void test_findcg_transform() {
    ccs::FindcgResponsesTransform transform;
    ccs::TaskConfig task{
        ccs::ApiTaskKind::Responses,
        true,
        "POST",
        "/v1/responses/",
        {"https://www.findcg.com", "/v1/responses/"},
        {"remove_findcg_image_gen"},
        true,
    };
    const std::string body = R"({"input":"image_gen remains text","tools":[{"type":"namespace","name":"image_gen"},{"type":"function","name":"image_gen"},{"namespace":"image_gen"},{"type":"function","name":"web_search"}],"nested":{"tools":[{"name":"image_gen"}]}})";
    auto result = transform.apply(task, body);
    require(result.matched, "findcg transform matched");
    require(result.modified, "findcg transform modified");
    require(result.removed_tools.size() == 3, "three image tools removed");
    require(result.rewritten_body.has_value(), "rewritten body exists");

    const auto rewritten = nlohmann::json::parse(*result.rewritten_body);
    require(rewritten["tools"].size() == 1, "other root tool retained");
    require(rewritten["tools"][0]["name"] == "web_search", "web search retained");
    require(rewritten["input"] == "image_gen remains text", "text retained");
    require(rewritten["nested"]["tools"].size() == 1, "nested tool retained");

    const auto no_tools = transform.apply(task, R"({"input":"hi"})");
    require(no_tools.matched && !no_tools.modified && !no_tools.rewritten_body, "no tools reuses input body");
    const auto invalid_tools_shape = transform.apply(task, R"({"tools":"image_gen"})");
    require(invalid_tools_shape.matched && !invalid_tools_shape.modified, "non-array tools remain transparent");

    task.upstream.base_url = "https://example.com";
    const auto transparent = transform.apply(task, "not json");
    require(!transparent.matched && !transparent.rewritten_body, "non-findcg avoids JSON parsing");

    task.upstream.base_url = "https://findcg.com";
    bool parse_failed = false;
    try {
        (void)transform.apply(task, "not json");
    } catch (const ccs::TransformError& ex) {
        parse_failed = ex.status_code() == 400;
    }
    require(parse_failed, "invalid findcg JSON fails closed");
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
        ccs::AppConfig config;
        config.log_path = path;
        config.log_flush_interval_ms = 50;
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
    ccs::AppConfig config;
    config.log_flush_interval_ms = 1;
    config.log_queue_capacity = 256;
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
    ccs::AppConfig config;
    config.log_flush_interval_ms = 1;
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
        ccs::AppConfig config;
        config.log_path = parent_file / "ccs-trans.log";
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

void test_runtime_metrics() {
    ccs::RuntimeMetrics metrics;
    metrics.connection_accepted(1, 1);
    metrics.connection_accepted(2, 2);
    metrics.worker_started(1);
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
    require(snapshot.stream_chunks_forwarded == 1, "stream chunk metric");
    require(snapshot.stream_bytes_forwarded == 512, "stream byte metric");
    require(snapshot.upstream_requests_failed == 1, "upstream failure metric");
    require(snapshot.upstream_response_header_timeouts == 1, "response header timeout metric");
    require(snapshot.upstream_stream_idle_timeouts == 1, "stream idle timeout metric");
    require(snapshot.upstream_total_timeouts == 1, "total timeout metric");
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
        {"findcg transform", test_findcg_transform},
        {"logger flush contract", test_logger_flush_contract},
        {"logger backpressure contract", test_logger_backpressure_contract},
        {"logger failure contract", test_logger_failure_contract},
        {"logger open failure contract", test_logger_open_failure_contract},
        {"runtime metrics", test_runtime_metrics},
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
