#include "config/config.hpp"
#include "core/task_router.hpp"
#include "core/url.hpp"
#include "logging/logger.hpp"
#include "transforms/findcg_responses_transform.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
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
    });
    require(split.ok, split.error);
    require(!split.config.usage.enabled, "Usage disabled without shared URL");
    require(split.config.responses.upstream.path == "/v2/responses", "canonical path wins");
    require(split.config.worker_threads == 4, "worker thread option");
    require(split.config.max_connections == 50, "max connection option");

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

void test_logger_flush_contract() {
    const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path()
        / ("ccs-trans-core-logger-test-" + std::to_string(nonce) + ".log");
    std::error_code ec;
    std::filesystem::remove(path, ec);

    {
        ccs::AppConfig config;
        config.log_path = path;
        config.log_flush_interval_ms = 100;
        ccs::Logger logger(config);
        std::string error;
        require(logger.open(error), error);
        logger.log("info", "queued_before_error", {});
        logger.log("error", "flush_now", {});
        const auto immediate = read_file(path);
        require(immediate.find("queued_before_error") != std::string::npos, "error flushes queued records");
        require(immediate.find("flush_now") != std::string::npos, "error record is durable on return");
        logger.log("info", "drain_on_destroy", {});
    }

    const auto final = read_file(path);
    require(final.find("drain_on_destroy") != std::string::npos, "destructor drains normal records");
    std::filesystem::remove(path, ec);
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"config resolution", test_config_resolution},
        {"URL and router", test_url_and_router},
        {"findcg transform", test_findcg_transform},
        {"logger flush contract", test_logger_flush_contract},
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
