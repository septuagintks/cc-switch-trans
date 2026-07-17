#include "config/application_config.hpp"
#include "config/config_document.hpp"
#include "core/version.hpp"

#include <nlohmann/json.hpp>

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

} // namespace

int main() {
    require(std::string_view(ccs::kVersion) == "0.7.0-dev",
            "development version is visible");

    auto document = ccs::make_default_application_config_document();
    document.application.listener.port = 16000;
    document.application.runtime.max_inflight_bytes = 768ULL * 1024 * 1024;
    std::string serialized;
    std::string error;
    require(ccs::serialize_application_config_document(document, serialized, error), error);
    const auto root = nlohmann::json::parse(serialized);
    require(root.at("schema_version") == "ccs-trans.config/v3", "v3 schema serialized");
    require(!root.contains("profiles"), "v3 application config excludes profiles");
    require(root.at("runtime").at("max_inflight_bytes") == 768ULL * 1024 * 1024,
            "v3 inflight budget serialized");

    ccs::ApplicationConfigDocument reparsed;
    require(ccs::parse_application_config_document(serialized, reparsed, error), error);
    require(reparsed == document, "v3 application config round-trips");
    std::string canonical;
    require(ccs::serialize_application_config_document(reparsed, canonical, error), error);
    require(canonical == serialized, "v3 application config is canonical");

    ccs::ConfigSchemaKind schema = ccs::ConfigSchemaKind::Unsupported;
    require(ccs::detect_config_schema(serialized, schema, error)
                && schema == ccs::ConfigSchemaKind::V3,
            "v3 schema detection");

    ccs::ConfigDocument legacy;
    std::string legacy_bytes;
    require(ccs::serialize_config_document(legacy, legacy_bytes, error), error);
    require(ccs::detect_config_schema(legacy_bytes, schema, error)
                && schema == ccs::ConfigSchemaKind::V2,
            "v2 schema detection");
    require(!ccs::parse_application_config_document(legacy_bytes, reparsed, error),
            "v2 is not silently parsed as v3");
    require(error.find("expected ccs-trans.config/v3") != std::string::npos,
            "v2 rejection names required migration target");

    auto invalid = root;
    invalid["profiles"] = nlohmann::json::object();
    require(!ccs::parse_application_config_document(invalid.dump(), reparsed, error),
            "v3 rejects profiles field");
    invalid = root;
    invalid.at("runtime").erase("max_inflight_bytes");
    require(!ccs::parse_application_config_document(invalid.dump(), reparsed, error),
            "v3 requires inflight budget");
    invalid = root;
    invalid.at("runtime")["max_inflight_bytes"] = 1024;
    require(!ccs::parse_application_config_document(invalid.dump(), reparsed, error),
            "v3 rejects undersized inflight budget");

    const auto duplicate = serialized.substr(0, serialized.size() - 2)
        + ",\n  \"listener\": {}\n}\n";
    require(!ccs::parse_application_config_document(duplicate, reparsed, error),
            "v3 rejects duplicate object keys");
    require(error.find("duplicate") != std::string::npos,
            "duplicate-key error is stable");

    std::cout << "application config tests passed\n";
    return 0;
}
