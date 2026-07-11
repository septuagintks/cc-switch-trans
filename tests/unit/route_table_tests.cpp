#include "config/config_document.hpp"
#include "config/runtime_compiler.hpp"
#include "core/url.hpp"
#include "routing/profile.hpp"
#include "routing/route_table.hpp"
#include "runtime/runtime_snapshot.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace {

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

ccs::ProfileDefinition make_profile(
    const std::string& protocol,
    const std::string& prefix,
    const std::string& base_url,
    bool enabled = true,
    bool usage = true) {
    ccs::ProfileDefinition profile;
    profile.enabled = enabled;
    profile.protocol = ccs::ProtocolId{protocol};
    profile.local.request_path = prefix + "/request";
    profile.upstream.base_url = base_url;
    profile.upstream.request_path = "/v1/request";
    if (usage) {
        profile.local.usage_path = prefix + "/usage";
        profile.upstream.usage_path = "/v1/usage";
    }
    return profile;
}

ccs::ConfigDocument multi_protocol_document() {
    auto document = ccs::make_default_config_document();
    auto responses = make_profile("responses", "/findcg", "https://responses.example.com");
    ccs::RuleDefinition enabled_rule;
    enabled_rule.id.value = "enabled-rule";
    enabled_rule.enabled = true;
    enabled_rule.type = "remove_tool";
    enabled_rule.options["tool"] = "image_gen";
    responses.rules.push_back(enabled_rule);
    auto disabled_rule = enabled_rule;
    disabled_rule.id.value = "disabled-rule";
    disabled_rule.enabled = false;
    responses.rules.push_back(disabled_rule);
    document.profiles.emplace("findcg", std::move(responses));
    document.profiles.emplace(
        "openrouter",
        make_profile("chat", "/openrouter", "https://openrouter.example.com/api"));
    document.profiles.emplace(
        "anthropic",
        make_profile("messages", "/anthropic", "https://anthropic.example.com"));
    return document;
}

ccs::RuntimeSnapshotPtr compile(
    const ccs::ConfigDocument& document,
    const ccs::RuntimeCompileOptions& options = {}) {
    ccs::RuntimeCompiler compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-runtime-root");
    ccs::RuntimeSnapshotPtr snapshot;
    std::string error;
    require(compiler.compile(document, options, snapshot, error), error);
    return snapshot;
}

void test_path_canonicalization() {
    std::string canonical;
    std::string error;
    require(ccs::canonicalize_http_path("/v1/responses/", canonical, error), error);
    require(canonical == "/v1/responses", "one trailing slash is canonicalized");
    require(ccs::canonicalize_http_path("/users/%7ealice/%3a", canonical, error), error);
    require(canonical == "/users/~alice/%3A", "unreserved escapes decode and retained escapes uppercase");
    require(ccs::canonicalize_http_path("/case/%41", canonical, error) && canonical == "/case/A",
        "encoded ASCII canonicalizes");

    const std::vector<std::pair<std::string, std::string>> invalid = {
        {"relative", "start with"},
        {"/v1//responses", "duplicate separators"},
        {"/v1/../responses", "dot segments"},
        {"/v1/%2e%2e/responses", "encode dot"},
        {"/v1/%2f/responses", "encode dot or path separators"},
        {"/v1/%00/responses", "encode control"},
        {"/v1/%zz", "invalid percent"},
        {"/v1/responses?x=1", "query"},
        {"/v1\\responses", "backslash"},
    };
    for (const auto& [path, expected] : invalid) {
        require(!ccs::canonicalize_http_path(path, canonical, error)
                && error.find(expected) != std::string::npos,
            "invalid path rejected: " + path);
    }
}

void test_route_table_lookup() {
    std::unordered_set<ccs::RouteKey, ccs::RouteKeyHash> keys;
    keys.insert(ccs::RouteKey{"POST", "/same"});
    keys.insert(ccs::RouteKey{"POST", "/same"});
    keys.insert(ccs::RouteKey{"GET", "/same"});
    require(keys.size() == 2, "RouteKey equality and hash include method and canonical path");

    auto profile = std::make_shared<ccs::RuntimeProfile>();
    profile->id = "synthetic";
    profile->protocol = ccs::ProtocolId{"responses"};
    std::shared_ptr<const ccs::RuntimeProfile> immutable_profile = profile;

    ccs::RouteTable table;
    std::string error;
    require(table.add(ccs::RouteEntry{
                immutable_profile,
                ccs::RouteKind::Request,
                "POST",
                "/same/%66oo/",
                {"https://example.com", "/request"},
            }, error),
        error);
    require(table.add(ccs::RouteEntry{
                immutable_profile,
                ccs::RouteKind::Usage,
                "GET",
                "/same/foo",
                {"https://example.com", "/usage"},
            }, error),
        error);
    require(table.size() == 2 && !table.empty(), "same canonical path supports two methods");

    const auto post = table.lookup("POST", "/same/foo");
    require(post.status == ccs::RouteLookupStatus::Matched, "POST route matched");
    require(post.entry && post.entry->kind == ccs::RouteKind::Request, "request route kind");
    require(post.entry->local_path == "/same/foo", "route stores canonical path");
    require(post.entry->upstream.path == "/request", "route owns upstream target");
    const auto get = table.lookup("GET", "/same/%66oo/");
    require(get.status == ccs::RouteLookupStatus::Matched
            && get.entry->kind == ccs::RouteKind::Usage,
        "GET route matched equivalent path");

    const auto wrong_method = table.lookup("PATCH", "/same/foo");
    require(wrong_method.status == ccs::RouteLookupStatus::MethodNotAllowed, "known path returns 405 class");
    require(wrong_method.allowed_methods == std::vector<std::string>({"GET", "POST"}),
        "allowed methods are stable and sorted");
    require(table.lookup("POST", "/unknown").status == ccs::RouteLookupStatus::NotFound,
        "unknown path returns 404 class");
    require(table.lookup("POST", "/same//foo").status == ccs::RouteLookupStatus::InvalidPath,
        "invalid request path is distinct from 404");
    require(table.lookup("post", "/same/foo").status == ccs::RouteLookupStatus::MethodNotAllowed,
        "HTTP method matching is case-sensitive");

    require(!table.add(ccs::RouteEntry{
                 immutable_profile,
                 ccs::RouteKind::Request,
                 "POST",
                 "/same/foo",
                 {"https://other.example.com", "/other"},
             }, error)
            && error.find("route collision") != std::string::npos,
        "canonical duplicate route rejected");
    require(!table.add(ccs::RouteEntry{
                 immutable_profile,
                 ccs::RouteKind::Request,
                 "POST",
                 "/%5Fccs-trans/health",
                 {"https://example.com", "/health"},
             }, error)
            && error.find("reserved") != std::string::npos,
        "encoded management namespace rejected after canonicalization");
    require(!table.add(ccs::RouteEntry{
                 immutable_profile,
                 ccs::RouteKind::Request,
                 "post",
                 "/lower-method",
                 {"https://example.com", "/request"},
             }, error)
            && error.find("uppercase") != std::string::npos,
        "invalid configured method rejected");
    require(std::string(ccs::route_kind_name(ccs::RouteKind::Request)) == "request", "request label");
    require(std::string(ccs::route_kind_name(ccs::RouteKind::Usage)) == "usage", "usage label");
}

void test_runtime_compiler_multi_profile() {
    auto document = multi_protocol_document();
    const auto snapshot = compile(document);
    require(snapshot->profiles.size() == 3, "three enabled profiles compiled");
    require(snapshot->routes.size() == 6, "request and Usage routes compiled");
    require(snapshot->log_path
            == std::filesystem::temp_directory_path()
                / "ccs-trans-runtime-root"
                / "logs/ccs-trans.log",
        "relative log path resolved under application root");

    auto absolute_log_document = multi_protocol_document();
    const auto absolute_log = std::filesystem::temp_directory_path() / "ccs-trans-external.log";
    absolute_log_document.application.logging.path = absolute_log.generic_string();
    require(compile(absolute_log_document)->log_path == absolute_log.lexically_normal(),
        "absolute log path remains outside the application root");

    const auto responses = snapshot->routes.lookup("POST", "/findcg/request/");
    require(responses.status == ccs::RouteLookupStatus::Matched, "Responses route lookup");
    require(responses.entry->profile->id == "findcg", "Responses profile ownership");
    require(responses.entry->profile->protocol.value == "responses", "Responses protocol ownership");
    require(responses.entry->profile->request_pipeline.size() == 1, "only enabled rules enter runtime pipeline");
    require(responses.entry->profile->request_pipeline[0].id.value == "enabled-rule", "rule order retained");
    require(responses.entry->upstream.base_url == "https://responses.example.com", "Responses upstream ownership");

    const auto usage = snapshot->routes.lookup("GET", "/findcg/usage");
    require(usage.status == ccs::RouteLookupStatus::Matched, "Usage route lookup");
    require(usage.entry->kind == ccs::RouteKind::Usage, "Usage route kind");
    require(usage.entry->upstream.path == "/v1/usage", "Usage follows profile upstream path");
    require(snapshot->routes.lookup("POST", "/openrouter/request").entry->profile->protocol.value == "chat",
        "Chat profile compiled without task enum");
    require(snapshot->routes.lookup("POST", "/anthropic/request").entry->profile->protocol.value == "messages",
        "Messages profile compiled without task enum");
    require(snapshot->routes.lookup("GET", "/anthropic/request").status
            == ccs::RouteLookupStatus::MethodNotAllowed,
        "known path wrong method classified");
    require(snapshot->routes.lookup("POST", "/FindCG/request").status == ccs::RouteLookupStatus::NotFound,
        "route paths are case-sensitive");
}

void test_collision_and_selected_profile() {
    ccs::RuntimeCompiler compiler(
        std::filesystem::temp_directory_path() / "ccs-trans-runtime-root");
    std::string error;
    ccs::RuntimeSnapshotPtr snapshot = compile(multi_protocol_document());
    const auto previous = snapshot;

    auto collision = ccs::make_default_config_document();
    collision.profiles.emplace("first", make_profile("responses", "/shared", "https://first.example.com"));
    auto second = make_profile("chat", "/other", "https://second.example.com");
    second.local.request_path = "/shared/request/";
    collision.profiles.emplace("second", std::move(second));
    require(!compiler.compile(collision, {}, snapshot, error)
            && error.find("route collision") != std::string::npos
            && error.find("first") != std::string::npos
            && error.find("second") != std::string::npos,
        "cross-profile canonical collision has actionable diagnostics");
    require(snapshot == previous, "failed compile leaves output snapshot unchanged");

    auto percent_collision = ccs::make_default_config_document();
    auto encoded = make_profile("responses", "/encoded", "https://encoded.example.com", true, false);
    encoded.local.request_path = "/same/%66oo";
    auto plain = make_profile("chat", "/plain", "https://plain.example.com", true, false);
    plain.local.request_path = "/same/foo";
    percent_collision.profiles.emplace("encoded", std::move(encoded));
    percent_collision.profiles.emplace("plain", std::move(plain));
    require(!compiler.compile(percent_collision, {}, snapshot, error)
            && error.find("route collision") != std::string::npos,
        "percent-equivalent routes collide");

    auto selected_document = ccs::make_default_config_document();
    selected_document.profiles.emplace(
        "draft",
        make_profile("responses", "/draft", "https://draft.example.com", false));
    ccs::RuntimeCompileOptions options;
    options.selected_profile = "draft";
    require(compiler.compile(selected_document, options, snapshot, error), error);
    require(snapshot->profiles.size() == 1, "selected disabled profile compiles alone");
    require(!snapshot->profiles.at("draft")->source_enabled, "diagnostic selection does not mutate enabled state");

    options.selected_profile = "missing";
    require(!compiler.compile(selected_document, options, snapshot, error)
            && error.find("does not exist") != std::string::npos,
        "missing selected profile rejected");
    options.selected_profile = "draft";
    selected_document.profiles.at("draft").upstream.request_path.reset();
    require(!compiler.compile(selected_document, options, snapshot, error)
            && error.find("not runnable") != std::string::npos,
        "incomplete selected draft rejected");

    auto empty = ccs::make_default_config_document();
    require(!compiler.compile(empty, {}, snapshot, error)
            && error.find("no enabled profiles") != std::string::npos,
        "default run requires an enabled profile");

    ccs::RuntimeCompiler relative_compiler("relative-root");
    require(!relative_compiler.compile(multi_protocol_document(), {}, snapshot, error)
            && error.find("absolute application root") != std::string::npos,
        "relative application root rejected before snapshot publication");
}

void test_snapshot_ownership_and_concurrent_lookup() {
    auto document = multi_protocol_document();
    const auto snapshot = compile(document);
    const auto original = snapshot->routes.lookup("POST", "/findcg/request");
    require(original.entry != nullptr, "ownership route exists");
    const auto original_profile = original.entry->profile;

    auto& editable = document.profiles.at("findcg");
    editable.local.request_path = "/mutated/request";
    editable.upstream.base_url = "https://mutated.example.com";
    editable.rules[0].options["tool"] = "mutated";
    document.profiles.clear();

    const auto retained = snapshot->routes.lookup("POST", "/findcg/request");
    require(retained.status == ccs::RouteLookupStatus::Matched, "snapshot route survives source mutation");
    require(retained.entry->upstream.base_url == "https://responses.example.com", "snapshot upstream is owned");
    require(retained.entry->profile == original_profile, "route shares immutable runtime profile");
    require(retained.entry->profile->request_pipeline[0].options.at("tool") == "image_gen",
        "snapshot rule options are owned");

    std::atomic_bool failed{false};
    std::vector<std::thread> readers;
    for (int thread = 0; thread < 8; ++thread) {
        readers.emplace_back([&]() {
            for (int iteration = 0; iteration < 2000; ++iteration) {
                const auto lookup = snapshot->routes.lookup("POST", "/openrouter/request/");
                if (lookup.status != ccs::RouteLookupStatus::Matched
                    || lookup.entry == nullptr
                    || lookup.entry->profile->id != "openrouter") {
                    failed.store(true, std::memory_order_relaxed);
                    return;
                }
            }
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    require(!failed.load(std::memory_order_relaxed), "immutable route table supports concurrent lookup");
}

} // namespace

int main() {
    const std::vector<std::pair<const char*, std::function<void()>>> tests = {
        {"path canonicalization", test_path_canonicalization},
        {"route table lookup", test_route_table_lookup},
        {"runtime compiler multi profile", test_runtime_compiler_multi_profile},
        {"collision and selected profile", test_collision_and_selected_profile},
        {"snapshot ownership and concurrent lookup", test_snapshot_ownership_and_concurrent_lookup},
    };
    try {
        for (const auto& [name, test] : tests) {
            test();
            std::cout << "ok: " << name << "\n";
        }
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "route table tests failed: " << ex.what() << "\n";
        return 1;
    }
}
