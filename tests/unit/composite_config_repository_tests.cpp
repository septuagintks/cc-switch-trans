#include "config/application_config.hpp"
#include "config/composite_config_repository.hpp"
#include "config/config_document.hpp"
#include "config/config_editing_service.hpp"
#include "core/sha256.hpp"
#include "storage/sqlite_profile_store.hpp"

#include "sqlite3.h"
#include "../support/canonical_temp.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

using Json = nlohmann::json;

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

class Fixture final {
public:
    explicit Fixture(std::string_view label) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = ccs::test::canonical_temp_directory()
            / ("ccs-trans-composite-" + std::string(label) + "-" + std::to_string(nonce));
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        require(!ec, "create test directory");
    }

    ~Fixture() {
#ifdef _WIN32
        std::error_code walk_error;
        if (std::filesystem::exists(root, walk_error)) {
            for (const auto& entry : std::filesystem::recursive_directory_iterator(
                     root,
                     std::filesystem::directory_options::skip_permission_denied,
                     walk_error)) {
                if (!walk_error && entry.is_regular_file()) {
                    (void)SetFileAttributesW(entry.path().c_str(), FILE_ATTRIBUTE_NORMAL);
                }
            }
        }
#endif
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::filesystem::path root;
};

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(static_cast<bool>(output), "open test file for writing");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(static_cast<bool>(output), "write test file");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(static_cast<bool>(input), "open test file for reading");
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

bool is_read_only(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES
        && (attributes & FILE_ATTRIBUTE_READONLY) != 0;
#else
    std::error_code ec;
    const auto permissions = std::filesystem::status(path, ec).permissions();
    return !ec
        && (permissions & std::filesystem::perms::owner_write)
            == std::filesystem::perms::none;
#endif
}

ccs::StoredProfile complete_profile(std::string id, std::string prefix) {
    ccs::StoredProfile profile;
    profile.profile_id = std::move(id);
    profile.enabled = true;
    profile.protocol = "responses";
    profile.local_request_path = "/" + prefix + "/v1/responses";
    profile.local_usage_path = "/" + prefix + "/v1/usage";
    profile.upstream_base_url = "https://example.test/v1";
    profile.upstream_request_path = "/responses";
    profile.upstream_usage_path = "/usage";
    return profile;
}

ccs::StoredRule remove_tool_rule(std::string id, std::string tool) {
    return ccs::StoredRule{
        0,
        std::move(id),
        true,
        "remove_tool",
        "{\"tool\":\"" + tool + "\"}",
    };
}

std::string serialize_application(const ccs::ApplicationSettings& settings) {
    std::string content;
    std::string error;
    require(
        ccs::serialize_application_config_document({settings}, content, error),
        error);
    return content;
}

void write_repository_journal(
    const ccs::AppPaths& paths,
    std::string_view old_config,
    std::string_view new_config,
    ccs::ProfileRevision old_revision,
    ccs::ProfileRevision target_revision) {
    std::error_code ec;
    std::filesystem::create_directories(paths.repository_transaction_directory, ec);
    require(!ec, "create repository journal directory");
    write_file(paths.repository_transaction_directory / "old-config.bin", old_config);
    write_file(paths.repository_transaction_directory / "new-config.json", new_config);
    const Json manifest = {
        {"schema_version", "ccs-trans.repository-transaction/v1"},
        {"kind", "commit"},
        {"old_config_exists", true},
        {"old_config_sha256", ccs::sha256_hex(old_config)},
        {"new_config_sha256", ccs::sha256_hex(new_config)},
        {"old_database_exists", true},
        {"old_profile_revision", old_revision},
        {"target_profile_revision", target_revision},
        {"old_migration_hash", nullptr},
        {"target_migration_hash", nullptr},
    };
    write_file(
        paths.repository_transaction_directory / "manifest.json",
        manifest.dump(2) + "\n");
}

void test_fresh_initialization_and_verify_is_non_mutating() {
    Fixture fixture("fresh");
    const auto paths = ccs::make_app_paths(fixture.root);
    ccs::CompositeConfigRepository repository(paths);
    ccs::StorageStatus status;
    std::string error;
    require(repository.inspect_storage(status, error), error);
    require(status.state == ccs::StorageState::Uninitialized,
            "fresh storage reports uninitialized");
    require(!repository.verify_storage(error), "verify rejects uninitialized storage");
    require(repository.last_failure() == ccs::ConfigRepositoryFailure::NotLoaded,
            "verify reports not loaded for uninitialized storage");
    require(!std::filesystem::exists(paths.config_file),
            "verify does not create config file");
    require(!std::filesystem::exists(paths.profiles_database),
            "verify does not create profile database");

    require(repository.load(error), error);
    require(repository.loaded(), "fresh repository loads");
    require(repository.snapshot().revision.application_source.exists,
            "fresh config has a source token");
    require(repository.snapshot().revision.profile_revision == 0,
            "fresh profile revision is zero");
    require(repository.snapshot().profiles.empty(), "fresh profiles are empty");
    require(repository.verify_storage(error), error);
}

void test_config_database_and_combined_saves() {
    Fixture fixture("save");
    const auto paths = ccs::make_app_paths(fixture.root);
    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(repository.load(error), error);

    auto database_only = repository.snapshot();
    auto beta = complete_profile("beta", "beta");
    beta.rules.push_back(remove_tool_rule("remove-image", "image_gen"));
    database_only.profiles.push_back(std::move(beta));
    ccs::ConfigurationSnapshot committed;
    require(repository.save_snapshot(database_only, committed, error), error);
    require(committed.revision.profile_revision == 1,
            "database-only save increments profile revision");
    require(committed.profiles.front().key > 0, "database assigns profile key");
    require(committed.profiles.front().rules.front().key > 0,
            "database assigns rule key");
    const auto beta_key = committed.profiles.front().key;
    const auto beta_rule_key = committed.profiles.front().rules.front().key;

    auto config_only = committed;
    config_only.application.listener.port = 16000;
    require(repository.save_snapshot(config_only, committed, error), error);
    require(committed.revision.profile_revision == 1,
            "config-only save preserves profile revision");
    require(committed.application.listener.port == 16000,
            "config-only save persists application settings");

    const auto stale = config_only;
    auto combined = committed;
    combined.application.runtime.worker_threads = 24;
    combined.profiles.front().profile_id = "renamed-beta";
    combined.profiles.front().rules.front().options_json = "{\"tool\":\"web_search\"}";
    combined.profiles.insert(combined.profiles.begin(), complete_profile("alpha", "alpha"));
    require(repository.save_snapshot(combined, committed, error), error);
    require(committed.revision.profile_revision == 2,
            "combined save increments profile revision once");
    require(committed.profiles.size() == 2, "combined save persists both profiles");
    require(committed.profiles[0].profile_id == "alpha",
            "combined save preserves requested profile order");
    require(committed.profiles[1].profile_id == "renamed-beta",
            "profile rename persists");
    require(committed.profiles[1].key == beta_key,
            "profile rename preserves stable key");
    require(committed.profiles[1].rules.front().key == beta_rule_key,
            "rule edit preserves stable key");

    ccs::ConfigurationSnapshot ignored;
    require(!repository.save_snapshot(stale, ignored, error),
            "stale composite save is rejected");
    require(repository.last_failure() == ccs::ConfigRepositoryFailure::Stale,
            "stale composite save has stable failure");

    ccs::CompositeConfigRepository reloaded(paths);
    require(reloaded.load(error), error);
    require(reloaded.snapshot() == committed, "combined snapshot round-trips");
}

ccs::ConfigDocument make_legacy_document() {
    auto document = ccs::make_default_config_document();
    document.application.listener.port = 17000;
    ccs::ProfileDefinition profile;
    profile.enabled = true;
    profile.protocol = ccs::ProtocolId{"responses"};
    profile.local.request_path = "/findcg/v1/responses";
    profile.local.usage_path = "/findcg/v1/usage";
    profile.upstream.base_url = "https://example.test/v1";
    profile.upstream.request_path = "/responses";
    profile.upstream.usage_path = "/usage";
    ccs::RuleDefinition rule;
    rule.id = ccs::RuleId{"remove-image"};
    rule.enabled = true;
    rule.type = "remove_tool";
    rule.options.emplace("tool", "image_gen");
    profile.rules.push_back(std::move(rule));
    document.profiles.emplace("findcg", std::move(profile));
    return document;
}

void test_explicit_v2_migration_and_provenance() {
    Fixture fixture("migration");
    const auto paths = ccs::make_app_paths(fixture.root);
    const auto legacy = make_legacy_document();
    std::string source;
    std::string error;
    require(ccs::serialize_config_document(legacy, source, error), error);
    write_file(paths.config_file, source);

    ccs::CompositeConfigRepository repository(paths);
    require(!repository.load(error), "v2 load requires explicit migration");
    require(repository.last_failure() == ccs::ConfigRepositoryFailure::MigrationRequired,
            "v2 load reports migration required");
    require(!std::filesystem::exists(paths.profiles_database),
            "v2 load does not create database");

    ccs::StorageStatus status;
    require(repository.inspect_storage(status, error), error);
    require(status.state == ccs::StorageState::MigrationRequired,
            "storage status reports migration required");

    ccs::MigrationOutcome outcome = ccs::MigrationOutcome::AlreadyMigrated;
    require(repository.migrate_v2(outcome, error), error);
    require(outcome == ccs::MigrationOutcome::Migrated, "v2 migration completes");
    const auto source_hash = ccs::sha256_hex(source);
    require(repository.snapshot().migrated_from_sha256 == source_hash,
            "migration stores source provenance");
    require(repository.snapshot().profiles.size() == 1,
            "migration imports profile");
    require(
        read_file(paths.migrations_directory / source_hash / "config-v2.json") == source,
        "migration backup preserves exact v2 bytes");
    require(std::filesystem::is_regular_file(
                paths.migrations_directory / source_hash / "manifest.json"),
            "migration manifest is retained");
    require(is_read_only(paths.migrations_directory / source_hash / "config-v2.json")
            && is_read_only(paths.migrations_directory / source_hash / "manifest.json"),
        "migration backup and manifest are read-only");

    require(repository.migrate_v2(outcome, error), error);
    require(outcome == ccs::MigrationOutcome::AlreadyMigrated,
            "repeated migration is idempotent");
    require(repository.verify_storage(error), error);
}

void prepare_recovery_case(
    const ccs::AppPaths& paths,
    bool config_target,
    bool database_target,
    std::string& old_config,
    std::string& new_config) {
    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(repository.load(error), error);
    const auto old = repository.snapshot();
    require(old.revision.profile_revision == 0, "recovery fixture starts at revision zero");
    old_config = old.revision.application_source.bytes;
    auto new_application = old.application;
    new_application.listener.port = 18000;
    new_config = serialize_application(new_application);

    if (config_target) {
        write_file(paths.config_file, new_config);
    }
    if (database_target) {
        ccs::SqliteProfileStore store(paths.profiles_database);
        ccs::ProfileStoreSnapshot desired;
        desired.revision = 0;
        desired.profiles.push_back(complete_profile("target", "target"));
        ccs::ProfileStoreSnapshot committed;
        require(store.save(desired, committed, error), error);
        require(committed.revision == 1, "recovery target database revision is one");
    }
    write_repository_journal(paths, old_config, new_config, 0, 1);
}

void test_recovery_matrix() {
    for (int state = 0; state < 4; ++state) {
        Fixture fixture("recovery-" + std::to_string(state));
        const auto paths = ccs::make_app_paths(fixture.root);
        const bool config_target = (state & 1) != 0;
        const bool database_target = (state & 2) != 0;
        std::string old_config;
        std::string new_config;
        prepare_recovery_case(
            paths, config_target, database_target, old_config, new_config);

        ccs::CompositeConfigRepository recovered(paths);
        std::string error;
        require(recovered.load(error), error);
        require(!std::filesystem::exists(paths.repository_transaction_directory),
                "successful recovery clears journal");
        require(
            read_file(paths.config_file) == (database_target ? new_config : old_config),
            "recovery config follows committed database state");
        require(
            recovered.snapshot().revision.profile_revision == (database_target ? 1 : 0),
            "recovery preserves database revision");
        require(
            recovered.snapshot().profiles.size() == (database_target ? 1U : 0U),
            "recovery preserves database contents");
    }
}

void test_ambiguous_recovery_is_rejected() {
    Fixture fixture("ambiguous");
    const auto paths = ccs::make_app_paths(fixture.root);
    std::string old_config;
    std::string new_config;
    prepare_recovery_case(paths, false, false, old_config, new_config);
    auto third_application = ccs::ApplicationSettings{};
    third_application.listener.port = 19000;
    write_file(paths.config_file, serialize_application(third_application));

    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(!repository.load(error), "ambiguous journal state is rejected");
    require(repository.last_failure() == ccs::ConfigRepositoryFailure::RecoveryRequired,
            "ambiguous journal state reports recovery required");
    require(std::filesystem::exists(paths.repository_transaction_directory),
            "ambiguous recovery retains journal for inspection");
}

void test_combined_save_rolls_back_config_when_database_is_busy() {
    Fixture fixture("busy-rollback");
    const auto paths = ccs::make_app_paths(fixture.root);
    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(repository.load(error), error);
    const auto before = repository.snapshot();

    sqlite3* database = nullptr;
    const auto encoded = path_to_utf8(paths.profiles_database);
    require(sqlite3_open_v2(
                encoded.c_str(),
                &database,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
                nullptr)
            == SQLITE_OK,
        "open raw database for lock contention");
    require(sqlite3_exec(database, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) == SQLITE_OK,
        "acquire raw database write transaction");

    auto desired = before;
    desired.application.listener.port = 20000;
    desired.profiles.push_back(complete_profile("blocked", "blocked"));
    ccs::ConfigurationSnapshot ignored;
    require(!repository.save_snapshot(desired, ignored, error),
        "combined save fails while profile database is write-locked");
    require(repository.last_failure() == ccs::ConfigRepositoryFailure::Busy,
        "database lock maps to repository busy");
    require(read_file(paths.config_file) == before.revision.application_source.bytes,
        "failed combined save restores exact old config bytes");
    require(!std::filesystem::exists(paths.repository_transaction_directory),
        "failed combined save clears recovered journal");

    require(sqlite3_exec(database, "ROLLBACK", nullptr, nullptr, nullptr) == SQLITE_OK,
        "release raw database transaction");
    require(sqlite3_close(database) == SQLITE_OK, "close raw database");

    ccs::CompositeConfigRepository reloaded(paths);
    require(reloaded.load(error), error);
    require(reloaded.snapshot() == before,
        "failed combined save leaves the repository snapshot unchanged");
}

void test_migration_refuses_existing_database() {
    Fixture fixture("migration-conflict");
    const auto paths = ccs::make_app_paths(fixture.root);
    const auto legacy = make_legacy_document();
    std::string source;
    std::string error;
    require(ccs::serialize_config_document(legacy, source, error), error);
    write_file(paths.config_file, source);
    ccs::SqliteProfileStore store(paths.profiles_database);
    ccs::ProfileStoreSnapshot empty;
    require(store.open_or_create(empty, error), error);

    ccs::CompositeConfigRepository repository(paths);
    ccs::MigrationOutcome outcome = ccs::MigrationOutcome::Migrated;
    require(!repository.migrate_v2(outcome, error),
        "migration refuses an existing profile database");
    require(repository.last_failure() == ccs::ConfigRepositoryFailure::Constraint,
        "existing profile database reports a migration constraint");
    require(read_file(paths.config_file) == source,
        "migration conflict preserves exact v2 source bytes");
    require(!std::filesystem::exists(paths.repository_transaction_directory),
        "migration conflict does not publish a journal");
}

void test_legacy_gui_adapter_preserves_profile_identity_only() {
    Fixture fixture("legacy-adapter");
    const auto paths = ccs::make_app_paths(fixture.root);
    ccs::CompositeConfigRepository repository(paths);
    std::string error;
    require(repository.load(error), error);
    auto desired = repository.snapshot();
    auto profile = complete_profile("zeta", "zeta");
    profile.rules.push_back(remove_tool_rule("old-rule", "image_gen"));
    desired.profiles.push_back(std::move(profile));
    desired.profiles.push_back(complete_profile("alpha", "alpha"));
    ccs::ConfigurationSnapshot committed;
    require(repository.save_snapshot(desired, committed, error), error);
    const auto profile_key = committed.profiles.front().key;
    const auto old_rule_key = committed.profiles.front().rules.front().key;

    ccs::ConfigEditingService editing(repository);
    require(editing.begin(error), error);
    auto node = editing.draft().profiles.extract("zeta");
    require(!node.empty(), "legacy draft contains persisted profile");
    node.key() = "omega";
    node.mapped().rules.front().id.value = "new-rule";
    editing.draft().profiles.insert(std::move(node));
    require(editing.commit(error), error);

    require(repository.snapshot().profiles.front().profile_id == "omega",
        "legacy GUI rename persists new profile id");
    require(repository.snapshot().profiles.front().key == profile_key,
        "legacy GUI rename preserves profile key metadata");
    require(repository.snapshot().profiles.front().rules.front().rule_id == "new-rule",
        "legacy Rule id replacement persists");
    require(repository.snapshot().profiles.front().rules.front().key != old_rule_key,
        "Rule id replacement receives a new key");
    require(repository.snapshot().profiles[1].profile_id == "alpha",
        "legacy GUI Apply preserves explicit Profile order instead of map order");
}

} // namespace

int main() {
    test_fresh_initialization_and_verify_is_non_mutating();
    test_config_database_and_combined_saves();
    test_explicit_v2_migration_and_provenance();
    test_recovery_matrix();
    test_ambiguous_recovery_is_rejected();
    test_combined_save_rolls_back_config_when_database_is_busy();
    test_migration_refuses_existing_database();
    test_legacy_gui_adapter_preserves_profile_identity_only();
    std::cout << "composite config repository tests passed\n";
    return 0;
}
