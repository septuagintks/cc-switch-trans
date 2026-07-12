#include "app/app_service.hpp"
#include "config/config_document.hpp"
#include "config/runtime_compiler.hpp"
#include "core/cancellation.hpp"
#include "core/runtime_metrics.hpp"
#include "core/url.hpp"
#include "logging/logger.hpp"
#include "server/server.hpp"
#include "transport/header_filter.hpp"
#include "transport/upstream_transport.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
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

struct ControlledSinkState {
    std::mutex mutex;
    std::condition_variable cv;
    std::string data;
    bool block_first_flush = false;
    bool first_flush_entered = false;
    bool release_first_flush = false;
    bool fail_flush = false;
    std::size_t flush_count = 0;
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
            state_->cv.wait(lock, [this]() { return state_->release_first_flush; });
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

void test_url_and_transport_boundary() {
    const auto parsed = ccs::parse_http_url("HTTPS://EXAMPLE.COM.:443/base");
    require(parsed.secure && parsed.host == "example.com" && parsed.port == 443,
        "upstream URL is normalized");
    require(ccs::join_url_path(parsed.base_path, "/v1/responses", "stream=true")
            == "/base/v1/responses?stream=true",
        "upstream path and query are joined once");
    std::string canonical;
    std::string error;
    require(ccs::canonicalize_http_path("/profile/v1/responses/", canonical, error)
            && canonical == "/profile/v1/responses",
        "local path canonicalization is shared");

    auto transport = ccs::make_upstream_transport(
        ccs::TimeoutConfig{}, 1024, std::make_shared<ccs::RuntimeMetrics>());
#ifdef _WIN32
    require(std::string(transport->proxy_mode()) == "windows_system",
        "Windows transport owns system proxy policy");
#else
    require(std::string(transport->proxy_mode()) == "unsupported",
        "unsupported platform transport remains explicit");
#endif
}

void test_hop_by_hop_header_filtering() {
    const ccs::Headers request = {
        {"Host", "127.0.0.1"},
        {"Content-Length", "2"},
        {"Connection", "keep-alive, X-Remove"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "keep-alive"},
        {"Proxy-Authorization", "Basic secret"},
        {"TE", "trailers"},
        {"Trailer", "X-Checksum"},
        {"Upgrade", "websocket"},
        {"X-Remove", "connection-scoped"},
        {"Authorization", "Bearer upstream-token"},
        {"X-Keep", "request-value"},
    };
    require(
        ccs::filter_request_headers(request)
            == ccs::Headers({
                {"Authorization", "Bearer upstream-token"},
                {"X-Keep", "request-value"},
            }),
        "request forwarding strips standard and Connection-nominated hop-by-hop headers");

    const ccs::Headers response = {
        {"Content-Length", "2"},
        {"Connection", "close, X-Remove"},
        {"Keep-Alive", "timeout=5"},
        {"Proxy-Connection", "close"},
        {"Proxy-Authenticate", "Basic realm=proxy"},
        {"Trailer", "X-Checksum"},
        {"Upgrade", "websocket"},
        {"X-Remove", "connection-scoped"},
        {"Content-Type", "application/json"},
        {"X-Keep", "response-value"},
    };
    require(
        ccs::filter_response_headers(response)
            == ccs::Headers({
                {"Content-Type", "application/json"},
                {"X-Keep", "response-value"},
            }),
        "response forwarding strips standard and Connection-nominated hop-by-hop headers");
}

void test_logger_durability_and_generation_metrics() {
    ccs::LoggerConfig config;
    config.flush_interval_ms = 50;
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    auto first_state = std::make_shared<ControlledSinkState>();
    {
        ccs::Logger first(
            config, metrics, std::make_unique<ControlledLogSink>(first_state));
        std::string error;
        require(first.open(error), error);
        require(first.log("info", "batched", {}), "normal record is accepted");
        require(first.drain(error), "explicit drain succeeds: " + error);
        {
            std::lock_guard<std::mutex> lock(first_state->mutex);
            require(first_state->data.find("\"event\":\"batched\"") != std::string::npos,
                "drain makes the record durable");
        }
        require(metrics->snapshot().log_writers_active == 1,
            "current generation writer is active");

        auto failed_state = std::make_shared<ControlledSinkState>();
        failed_state->fail_flush = true;
        std::string reported_failure;
        {
            ccs::Logger failed(
                config,
                metrics,
                std::make_unique<ControlledLogSink>(failed_state),
                [&](const std::string& callback_error) {
                    reported_failure = callback_error;
                });
            require(failed.open(error), error);
            require(metrics->snapshot().log_writers_active == 2,
                "overlapping generations count both writers");
            require(!failed.log("error", "must_fail", {}),
                "failed durability is reported to the caller");
            const auto overlap = metrics->snapshot();
            require(overlap.log_writer_failures == 1
                    && overlap.log_writers_active == 1
                    && overlap.log_writer_healthy == 1,
                "one failed generation does not hide the healthy writer");
            require(reported_failure == "injected flush failure",
                "host receives the writer failure reason");
        }
    }
    const auto stopped = metrics->snapshot();
    require(stopped.log_writers_active == 0 && stopped.log_writer_healthy == 0,
        "retiring all generations clears writer health");
}

void test_logger_backpressure() {
    ccs::LoggerConfig config;
    config.flush_interval_ms = 1;
    config.queue_capacity = 256;
    auto metrics = std::make_shared<ccs::RuntimeMetrics>();
    auto state = std::make_shared<ControlledSinkState>();
    state->block_first_flush = true;
    ccs::Logger logger(config, metrics, std::make_unique<ControlledLogSink>(state));
    std::string error;
    require(logger.open(error), error);
    require(logger.log("info", "fills_capacity", {
        ccs::field_string("body", std::string(512, 'x')),
    }), "one oversized record may occupy the bounded queue");
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        require(state->cv.wait_for(lock, std::chrono::milliseconds(500), [&]() {
            return state->first_flush_entered;
        }), "writer entered the controlled flush");
    }

    std::atomic_bool producer_finished{false};
    std::atomic_bool producer_accepted{false};
    std::thread producer([&]() {
        producer_accepted.store(logger.log("info", "waits_for_capacity", {}));
        producer_finished.store(true);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    require(!producer_finished.load(), "producer waits while queue capacity is retained");
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->release_first_flush = true;
    }
    state->cv.notify_all();
    producer.join();
    require(producer_accepted.load(), "producer resumes after durable flush");
    const auto snapshot = metrics->snapshot();
    require(snapshot.log_backpressure_count == 1
            && snapshot.log_backpressure_wait_us > 0,
        "logger backpressure is measured rather than dropping records");
}

void test_runtime_metrics() {
    ccs::RuntimeMetrics metrics;
    metrics.connection_accepted(2, 2);
    metrics.worker_started(1, 25);
    metrics.request_started();
    metrics.stream_started();
    metrics.stream_chunk_forwarded(512);
    metrics.upstream_request_started();
    metrics.upstream_request_failed();
    metrics.upstream_timeout(ccs::UpstreamTimeoutPhase::ResponseHeader);
    metrics.request_completed();
    metrics.worker_finished(1);
    metrics.connection_rejected();

    const auto snapshot = metrics.snapshot();
    require(snapshot.connections_accepted == 1
            && snapshot.connections_rejected == 1
            && snapshot.connections_completed == 1,
        "global connection counters are coherent");
    require(snapshot.peak_connections == 2
            && snapshot.peak_queued_connections == 2
            && snapshot.peak_active_workers == 1,
        "global resource high-water marks are coherent");
    require(snapshot.stream_chunks_forwarded == 1
            && snapshot.stream_bytes_forwarded == 512,
        "stream counters retain byte totals");
    require(snapshot.upstream_requests_failed == 1
            && snapshot.upstream_response_header_timeouts == 1,
        "upstream failures and timeout phases are classified");
}

void test_server_stops_on_logger_failure() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto port = static_cast<std::uint16_t>(40000 + nonce % 10000);
    auto document = runtime_document(
        port,
        std::filesystem::temp_directory_path() / "ccs-trans-unused-controlled.log");
    document.application.logging.flush_interval_ms = 1;
    auto state = std::make_shared<ControlledSinkState>();
    state->fail_flush = true;
    ccs::Server server(
        compile_runtime(document),
        [state]() { return std::make_unique<ControlledLogSink>(state); });

    bool startup_reported = false;
    bool startup_succeeded = true;
    std::string startup_error;
    const int exit_code = server.run([&](bool succeeded, const std::string& error) {
        startup_reported = true;
        startup_succeeded = succeeded;
        startup_error = error;
    });
    require(exit_code != 0 && startup_reported && !startup_succeeded,
        "writer failure prevents a successful server startup");
    require(startup_error == "injected flush failure",
        "server reports the durability failure");
}

void test_app_service_startup_failure() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto log_path = std::filesystem::temp_directory_path()
        / ("ccs-trans-service-startup-" + std::to_string(nonce) + ".log");
    ccs::AppService service(compile_runtime(runtime_document(
        15723, log_path, "https://example.com", "256.256.256.256")));
    std::string error;
    require(!service.start(error)
            && error.find("failed to resolve listen address") != std::string::npos,
        "service startup failure is synchronous and actionable");
    int exit_code = 0;
    require(service.status() == ccs::ServiceState::Stopped
            && service.try_wait(exit_code)
            && exit_code != 0,
        "failed service can be reaped without blocking and retains a non-zero exit");
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
    auto hot = document;
    hot.profiles.at("primary").upstream.base_url = "https://responses-b.example.com";
    require(server.reload(compile_runtime(hot), error) == ccs::ReloadResult::Applied,
        "upstream-only reload is applied in place: " + error);

    auto worker_restart = hot;
    ++worker_restart.application.runtime.worker_threads;
    require(server.reload(compile_runtime(worker_restart), error)
            == ccs::ReloadResult::RestartRequired,
        "worker topology requires restart");

    auto writer_restart = hot;
    ++writer_restart.application.logging.flush_interval_ms;
    require(server.reload(compile_runtime(writer_restart), error)
            == ccs::ReloadResult::RestartRequired,
        "same-path writer topology requires restart");

    auto hot_limit = hot;
    --hot_limit.application.runtime.max_response_body_size;
    require(server.reload(compile_runtime(hot_limit), error) == ccs::ReloadResult::Applied,
        "generation-owned response limit reloads in place");
    require(server.reload({}, error) == ccs::ReloadResult::Failed && !error.empty(),
        "null snapshot is rejected");
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
}

void test_app_service_restart_and_rollback() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto port = static_cast<std::uint16_t>(30000 + nonce % 10000);
    const auto log_path = std::filesystem::temp_directory_path()
        / ("ccs-trans-service-reload-" + std::to_string(nonce) + ".log");
    auto document = runtime_document(port, log_path, "https://responses-a.example.com");
    ccs::AppService service(compile_runtime(document));
    std::string error;
    require(service.start(error), "reload test service starts: " + error);
    int early_exit_code = 0;
    require(!service.try_wait(early_exit_code), "running service cannot be reaped");

    auto restart = document;
    ++restart.application.runtime.worker_threads;
    require(service.reload(compile_runtime(restart), error),
        "service performs graceful restart reload: " + error);

    auto invalid = restart;
    invalid.application.listener.host = "256.256.256.256";
    require(!service.reload(compile_runtime(invalid), error)
            && error.find("previous configuration was restored") != std::string::npos,
        "failed restart restores the previous snapshot");
    require(service.status() == ccs::ServiceState::Running,
        "rollback restores a running service");
    service.stop();
    require(service.wait() == 0, "reloaded service stops cleanly");
    std::error_code ec;
    std::filesystem::remove(log_path, ec);
}

void test_server_local_protocol_errors() {
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

    const auto responses_error = send("POST", "/v1/responses", "not json");
    require(responses_error.find("HTTP/1.1 400") == 0
            && response_body(responses_error)["error"]["type"] == "invalid_request_error",
        "Responses uses the OpenAI local error envelope");
    const auto messages_error = send("POST", "/v1/messages", "[]");
    require(messages_error.find("HTTP/1.1 400") == 0
            && response_body(messages_error)["type"] == "error",
        "Messages uses the Anthropic local error envelope");
    require(send("GET", "/v1/responses", "").find("HTTP/1.1 405") == 0,
        "known path with wrong method returns 405");
    require(send("POST", "/unknown", "{}").find("HTTP/1.1 404") == 0,
        "unknown path returns 404");
    const auto malformed_http = server.process_raw_request(
            "POST /v1/responses HTTP/1.1\r\nMalformed-Header\r\n\r\n",
            "127.0.0.1");
    require(
        malformed_http.find("HTTP/1.1 400") == 0
            && response_body(malformed_http)["error"]["message"]
                == "request contains a malformed HTTP header",
        "malformed HTTP syntax is classified as a client error");
}

void test_cancellation() {
    ccs::CancellationSource source;
    const auto token = source.token();
    int callback_count = 0;
    auto registration = token.on_cancel([&]() { ++callback_count; });
    (void)registration;
    require(source.cancel() && token.is_cancelled() && callback_count == 1,
        "first cancellation publishes exactly once");
    require(!source.cancel(), "cancellation is idempotent");
    int immediate_count = 0;
    auto immediate = token.on_cancel([&]() { ++immediate_count; });
    (void)immediate;
    require(immediate_count == 1, "late callback runs immediately");

    ccs::CancellationSource long_lived_source;
    const auto long_lived_token = long_lived_source.token();
    std::vector<std::weak_ptr<int>> retired_callbacks;
    for (int index = 0; index < 128; ++index) {
        auto lifetime = std::make_shared<int>(index);
        retired_callbacks.emplace_back(lifetime);
        {
            auto transient = long_lived_token.on_cancel([lifetime]() {});
            (void)transient;
        }
    }
    require(
        std::all_of(
            retired_callbacks.begin(),
            std::prev(retired_callbacks.end()),
            [](const std::weak_ptr<int>& lifetime) { return lifetime.expired(); }),
        "inactive callbacks are reclaimed while a cancellation token remains alive");
    require(long_lived_source.cancel(), "long-lived source cancels once");
    require(
        std::all_of(
            retired_callbacks.begin(),
            retired_callbacks.end(),
            [](const std::weak_ptr<int>& lifetime) { return lifetime.expired(); }),
        "cancellation releases the final registered callback");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"URL and transport boundary", test_url_and_transport_boundary},
        {"hop-by-hop header filtering", test_hop_by_hop_header_filtering},
        {"logger durability and generation metrics", test_logger_durability_and_generation_metrics},
        {"logger backpressure", test_logger_backpressure},
        {"runtime metrics", test_runtime_metrics},
        {"server stops on logger failure", test_server_stops_on_logger_failure},
        {"app service startup failure", test_app_service_startup_failure},
        {"server reload classification", test_server_reload_classification},
        {"app service restart and rollback", test_app_service_restart_and_rollback},
        {"server local protocol errors", test_server_local_protocol_errors},
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
