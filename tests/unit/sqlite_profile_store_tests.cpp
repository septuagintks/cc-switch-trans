#include "config/app_paths.hpp"
#include "storage/sqlite_profile_store.hpp"

#include "sqlite3.h"
#include "../support/canonical_temp.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class Fixture final {
public:
    explicit Fixture(std::string_view label) {
        const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
        root = ccs::test::canonical_temp_directory()
            / ("ccs-trans-sqlite-store-" + std::string(label) + "-" + std::to_string(nonce));
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        require(!ec, "create test directory");
    }

    ~Fixture() {
        std::error_code ec;
        std::filesystem::remove_all(root, ec);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    std::filesystem::path root;
};

class RawDatabase final {
public:
    explicit RawDatabase(const std::filesystem::path& path) {
        const auto encoded = path_to_utf8(path);
        require(
            sqlite3_open_v2(
                encoded.c_str(),
                &database_,
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW,
                nullptr)
                == SQLITE_OK,
            "open raw SQLite database");
        require(sqlite3_extended_result_codes(database_, 1) == SQLITE_OK,
                "enable raw extended result codes");
        require(sqlite3_busy_timeout(database_, 100) == SQLITE_OK,
                "configure raw busy timeout");
        require(sqlite3_exec(database_, "PRAGMA foreign_keys=ON", nullptr, nullptr, nullptr)
                    == SQLITE_OK,
                "enable raw foreign keys");
    }

    ~RawDatabase() {
        if (database_ != nullptr) {
            sqlite3_close(database_);
        }
    }

    sqlite3* get() const noexcept {
        return database_;
    }

private:
    sqlite3* database_ = nullptr;
};

std::int64_t scalar_integer(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK,
            sqlite3_errmsg(database));
    require(sqlite3_step(statement) == SQLITE_ROW, sqlite3_errmsg(database));
    const auto value = sqlite3_column_int64(statement, 0);
    require(sqlite3_step(statement) == SQLITE_DONE, "scalar query returns one row");
    require(sqlite3_finalize(statement) == SQLITE_OK, "finalize scalar statement");
    return value;
}

std::string scalar_text(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK,
            sqlite3_errmsg(database));
    require(sqlite3_step(statement) == SQLITE_ROW, sqlite3_errmsg(database));
    const auto* value = sqlite3_column_text(statement, 0);
    require(value != nullptr, "scalar text is not null");
    std::string result(reinterpret_cast<const char*>(value));
    require(sqlite3_step(statement) == SQLITE_DONE, "scalar query returns one row");
    require(sqlite3_finalize(statement) == SQLITE_OK, "finalize scalar statement");
    return result;
}

void raw_exec(sqlite3* database, const char* sql) {
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, nullptr);
    require(result == SQLITE_OK, sqlite3_errmsg(database));
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

ccs::ProfileStoreSnapshot create_empty_store(
    const std::filesystem::path& path,
    ccs::SqliteProfileStoreOptions options = {}) {
    ccs::SqliteProfileStore store(path, options);
    ccs::ProfileStoreSnapshot snapshot;
    std::string error;
    const bool opened = store.open_or_create(snapshot, error);
    require(opened, error);
    require(snapshot.revision == 0, "new store revision is zero");
    require(snapshot.profiles.empty(), "new store is empty");
    return snapshot;
}

void test_paths_schema_and_pragmas() {
    Fixture fixture("schema");
    const auto paths = ccs::make_app_paths(fixture.root);
    require(paths.profiles_database == fixture.root / "profiles.db",
            "profile database path is fixed under application root");

    const auto created = create_empty_store(paths.profiles_database);
    require(std::filesystem::is_regular_file(paths.profiles_database),
            "profile database was created");

    ccs::SqliteProfileStore store(paths.profiles_database);
    ccs::ProfileStoreSnapshot loaded;
    std::string error;
    require(store.open_or_create(loaded, error), error);
    require(loaded == created, "open_or_create reloads existing database");
    require(store.verify(error), error);

    RawDatabase raw(paths.profiles_database);
    require(scalar_text(raw.get(), "PRAGMA journal_mode") == "wal", "WAL mode persisted");
    require(scalar_integer(raw.get(), "PRAGMA page_size") == 4096, "page size is fixed");
    require(scalar_integer(raw.get(), "PRAGMA foreign_keys") == 1,
            "foreign keys are enabled");
    require(scalar_integer(raw.get(), "PRAGMA user_version") == 1,
            "schema user_version is one");
    require(
        scalar_integer(
            raw.get(),
            "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN ("
            "'repository_meta','schema_history','migration_history','profiles','rules')")
            == 5,
        "all schema tables exist");
    require(
        scalar_integer(raw.get(), "SELECT revision FROM repository_meta WHERE singleton=1") == 0,
        "metadata revision is zero");
}

void test_crud_revision_and_stable_keys() {
    Fixture fixture("crud");
    const auto database_path = fixture.root / "profiles.db";
    auto draft = create_empty_store(database_path);
    ccs::SqliteProfileStore store(database_path);

    auto findcg = complete_profile("findcg", "findcg");
    findcg.rules.push_back(remove_tool_rule("remove-image", "image_gen"));
    findcg.rules.push_back(remove_tool_rule("remove-web", "web_search"));
    ccs::StoredProfile disabled;
    disabled.profile_id = "draft.profile";
    draft.profiles = {findcg, disabled};

    ccs::ProfileStoreSnapshot created;
    std::string error;
    require(store.save(draft, created, error), error);
    require(created.revision == 1, "create increments revision once");
    require(created.profiles.size() == 2, "two profiles created");
    require(created.profiles[0].key > 0 && created.profiles[1].key > 0,
            "profiles receive stable keys");
    require(created.profiles[0].rules[0].key > 0
                && created.profiles[0].rules[1].key > 0,
            "rules receive stable keys");
    const auto findcg_key = created.profiles[0].key;
    const auto disabled_key = created.profiles[1].key;
    const auto image_rule_key = created.profiles[0].rules[0].key;
    const auto web_rule_key = created.profiles[0].rules[1].key;

    ccs::ProfileStoreSnapshot loaded;
    require(store.load(loaded, error), error);
    require(loaded == created, "created snapshot round-trips");

    auto edited = created;
    std::swap(edited.profiles[0], edited.profiles[1]);
    edited.profiles[0].profile_id = "backup.profile";
    auto& edited_findcg = edited.profiles[1];
    edited_findcg.upstream_base_url = "https://api.example.test/v1";
    std::swap(edited_findcg.rules[0], edited_findcg.rules[1]);
    edited_findcg.rules[1].options_json = "{\"tool\":\"image_generation\"}";
    edited_findcg.rules.push_back(remove_tool_rule("remove-code", "code_interpreter"));

    ccs::ProfileStoreSnapshot updated;
    require(store.save(edited, updated, error), error);
    require(updated.revision == 2, "update increments revision once");
    require(updated.profiles[0].key == disabled_key
                && updated.profiles[1].key == findcg_key,
            "profile reorder and rename preserve stable keys");
    require(updated.profiles[1].rules[0].key == web_rule_key
                && updated.profiles[1].rules[1].key == image_rule_key,
            "rule reorder and update preserve stable keys");
    require(updated.profiles[1].rules[2].key > web_rule_key,
            "new rule receives a new key");
    const auto newest_rule_key = updated.profiles[1].rules[2].key;

    ccs::ProfileStoreSnapshot no_op;
    require(store.save(updated, no_op, error), error);
    require(no_op == updated, "no-op save preserves revision and content");

    ccs::ProfileStoreSnapshot stale_result;
    require(!store.save(created, stale_result, error), "stale save is rejected");
    require(store.last_failure() == ccs::ProfileStoreFailure::Stale,
            "stale save has stable failure type");
    require(store.load(loaded, error) && loaded == updated,
            "stale save leaves database unchanged");

    auto deleted = updated;
    deleted.profiles.clear();
    ccs::ProfileStoreSnapshot empty;
    require(store.save(deleted, empty, error), error);
    require(empty.revision == 3 && empty.profiles.empty(),
            "delete all profiles commits once");

    auto recreated_draft = empty;
    auto recreated = complete_profile("findcg", "findcg");
    recreated.rules.push_back(remove_tool_rule("remove-image", "image_gen"));
    recreated_draft.profiles.push_back(std::move(recreated));
    ccs::ProfileStoreSnapshot recreated_result;
    require(store.save(recreated_draft, recreated_result, error), error);
    require(recreated_result.profiles[0].key > disabled_key,
            "deleted profile keys are never reused");
    require(recreated_result.profiles[0].rules[0].key > newest_rule_key,
            "deleted rule keys are never reused");
}

void test_constraints_busy_and_invalid_data() {
    Fixture fixture("failure");
    const auto database_path = fixture.root / "profiles.db";
    const auto empty = create_empty_store(database_path);

    {
        RawDatabase raw(database_path);
        raw_exec(
            raw.get(),
            "INSERT INTO profiles(profile_id,position,enabled) VALUES ('one',0,0)");
        const int duplicate = sqlite3_exec(
            raw.get(),
            "INSERT INTO profiles(profile_id,position,enabled) VALUES ('two',0,0)",
            nullptr,
            nullptr,
            nullptr);
        require((duplicate & 0xff) == SQLITE_CONSTRAINT,
                "profile position uniqueness is enforced");
        const int missing_parent = sqlite3_exec(
            raw.get(),
            "INSERT INTO rules(profile_key,rule_id,position,enabled,type,options_json) "
            "VALUES (999,'rule',0,0,'remove_tool','{}')",
            nullptr,
            nullptr,
            nullptr);
        require((missing_parent & 0xff) == SQLITE_CONSTRAINT,
                "rule foreign key is enforced");
        const int invalid_json = sqlite3_exec(
            raw.get(),
            "INSERT INTO rules(profile_key,rule_id,position,enabled,type,options_json) "
            "VALUES (1,'rule',0,0,'remove_tool','invalid')",
            nullptr,
            nullptr,
            nullptr);
        require((invalid_json & 0xff) == SQLITE_CONSTRAINT,
                "rule options JSON check is enforced");
        raw_exec(raw.get(), "DELETE FROM profiles");
    }

    ccs::SqliteProfileStore store(database_path);
    std::string error;
    auto invalid = empty;
    auto profile = complete_profile("findcg", "findcg");
    auto rule = remove_tool_rule("remove-image", "image_gen");
    rule.options_json = "{ \"tool\": \"image_gen\" }";
    profile.rules.push_back(std::move(rule));
    invalid.profiles.push_back(std::move(profile));
    ccs::ProfileStoreSnapshot result;
    require(!store.save(invalid, result, error), "non-canonical options are rejected");
    require(store.last_failure() == ccs::ProfileStoreFailure::InvalidData,
            "non-canonical options have stable failure type");

    invalid = empty;
    profile = complete_profile("findcg", "findcg");
    rule = remove_tool_rule("remove-image", "image_gen");
    rule.options_json = "{\"value\":\""
        + std::string(ccs::kMaxStoredRuleOptionsBytes, 'x') + "\"}";
    profile.rules.push_back(std::move(rule));
    invalid.profiles.push_back(std::move(profile));
    require(!store.save(invalid, result, error), "oversized rule options are rejected");
    require(store.last_failure() == ccs::ProfileStoreFailure::InvalidData,
            "oversized options have stable failure type");

    invalid = empty;
    for (std::size_t index = 0; index < 129; ++index) {
        ccs::StoredProfile item;
        item.profile_id = "profile." + std::to_string(index);
        invalid.profiles.push_back(std::move(item));
    }
    require(!store.save(invalid, result, error), "profile count limit is enforced");

    invalid = empty;
    profile = complete_profile("findcg", "findcg");
    profile.key = 999;
    invalid.profiles.push_back(std::move(profile));
    require(!store.save(invalid, result, error), "unknown stable key is rejected");
    require(store.last_failure() == ccs::ProfileStoreFailure::Stale,
            "unknown stable key is stale");

    RawDatabase lock(database_path);
    raw_exec(lock.get(), "BEGIN IMMEDIATE");
    auto blocked = empty;
    blocked.profiles.push_back(complete_profile("findcg", "findcg"));
    ccs::SqliteProfileStore busy_store(
        database_path,
        ccs::SqliteProfileStoreOptions{25, true});
    require(!busy_store.save(blocked, result, error), "writer lock contention is rejected");
    require(busy_store.last_failure() == ccs::ProfileStoreFailure::Busy,
            "writer lock contention has stable failure type");
    raw_exec(lock.get(), "ROLLBACK");
    ccs::ProfileStoreSnapshot loaded;
    require(store.load(loaded, error) && loaded == empty,
            "failed and busy saves leave database unchanged");
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    require(output.is_open(), "open fixture file");
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    require(output.good(), "write fixture file");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    require(input.is_open(), "read fixture file");
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

void test_corruption_and_unsupported_schema() {
    Fixture corrupt_fixture("corrupt");
    const auto corrupt_path = corrupt_fixture.root / "profiles.db";
    const std::string corrupt_bytes = "not a sqlite database";
    write_file(corrupt_path, corrupt_bytes);
    ccs::SqliteProfileStore corrupt_store(corrupt_path);
    ccs::ProfileStoreSnapshot snapshot;
    std::string error;
    require(!corrupt_store.open_or_create(snapshot, error), "corrupt database is rejected");
    require(corrupt_store.last_failure() == ccs::ProfileStoreFailure::Corrupt,
            "corrupt database has stable failure type");
    require(read_file(corrupt_path) == corrupt_bytes,
            "corrupt database is never overwritten with an empty store");

    Fixture unsupported_fixture("unsupported");
    const auto unsupported_path = unsupported_fixture.root / "profiles.db";
    create_empty_store(unsupported_path);
    {
        RawDatabase raw(unsupported_path);
        raw_exec(raw.get(), "UPDATE repository_meta SET schema_version=2 WHERE singleton=1");
    }
    ccs::SqliteProfileStore unsupported_store(unsupported_path);
    require(!unsupported_store.load(snapshot, error), "future schema is rejected");
    require(unsupported_store.last_failure() == ccs::ProfileStoreFailure::UnsupportedSchema,
            "future schema has stable failure type");

    Fixture blank_fixture("blank-schema");
    const auto blank_path = blank_fixture.root / "profiles.db";
    {
        const auto encoded = path_to_utf8(blank_path);
        sqlite3* database = nullptr;
        require(sqlite3_open(encoded.c_str(), &database) == SQLITE_OK,
                "create blank SQLite database");
        raw_exec(database, "CREATE TABLE unrelated(value INTEGER)");
        require(sqlite3_close(database) == SQLITE_OK, "close blank SQLite database");
    }
    ccs::SqliteProfileStore blank_store(blank_path);
    require(!blank_store.open_or_create(snapshot, error), "blank foreign schema is rejected");
    require(blank_store.last_failure() == ccs::ProfileStoreFailure::UnsupportedSchema,
            "blank foreign schema has stable failure type");

    Fixture extra_fixture("extra-schema");
    const auto extra_path = extra_fixture.root / "profiles.db";
    create_empty_store(extra_path);
    {
        RawDatabase raw(extra_path);
        raw_exec(raw.get(), "CREATE TABLE unexpected(value INTEGER)");
    }
    ccs::SqliteProfileStore extra_store(extra_path);
    require(!extra_store.load(snapshot, error), "unexpected schema object is rejected");
    require(extra_store.last_failure() == ccs::ProfileStoreFailure::UnsupportedSchema,
            "unexpected schema object has stable failure type");

    Fixture semantic_fixture("semantic-corrupt");
    const auto semantic_path = semantic_fixture.root / "profiles.db";
    create_empty_store(semantic_path);
    {
        RawDatabase raw(semantic_path);
        raw_exec(
            raw.get(),
            "INSERT INTO profiles(profile_id,position,enabled) VALUES ('profile',0,0)");
        raw_exec(
            raw.get(),
            "INSERT INTO rules(profile_key,rule_id,position,enabled,type,options_json) "
            "VALUES (1,'rule',0,0,'remove_tool','{ }')");
    }
    ccs::SqliteProfileStore semantic_store(semantic_path);
    require(!semantic_store.load(snapshot, error), "non-canonical persisted JSON is rejected");
    require(semantic_store.last_failure() == ccs::ProfileStoreFailure::Corrupt,
            "persisted semantic violation is classified as corruption");
}

void test_migration_provenance_and_checkpoint() {
    Fixture fixture("migration");
    const auto database_path = fixture.root / "profiles.db";
    auto draft = create_empty_store(database_path);
    draft.profiles.push_back(complete_profile("findcg", "findcg"));
    ccs::SqliteProfileStore store(database_path);
    ccs::ProfileStoreSnapshot saved;
    std::string error;
    require(store.save(draft, saved, error), error);
    const auto revision = saved.revision;

    const std::string source_hash(64, 'a');
    ccs::ProfileStoreSnapshot migrated;
    require(
        store.mark_migrated("ccs-trans.config/v2", source_hash, migrated, error),
        error);
    require(migrated.revision == revision,
            "migration provenance does not change profile revision");
    require(migrated.migrated_from_sha256 == source_hash,
            "migration provenance round-trips");
    {
        RawDatabase raw(database_path);
        require(
            scalar_integer(raw.get(), "SELECT count(*) FROM migration_history") == 1,
            "migration history records one source");
    }

    ccs::ProfileStoreSnapshot repeated;
    require(
        store.mark_migrated("ccs-trans.config/v2", source_hash, repeated, error),
        error);
    require(repeated == migrated, "same migration provenance is idempotent");
    require(
        !store.mark_migrated(
            "ccs-trans.config/v2", std::string(64, 'b'), repeated, error),
        "different migration provenance is rejected");
    require(store.last_failure() == ccs::ProfileStoreFailure::Constraint,
            "different migration provenance has stable failure type");

    require(store.checkpoint_for_move(error), error);
    require(store.verify(error), error);
}

} // namespace

int main() {
    test_paths_schema_and_pragmas();
    test_crud_revision_and_stable_keys();
    test_constraints_busy_and_invalid_data();
    test_corruption_and_unsupported_schema();
    test_migration_provenance_and_checkpoint();
    std::cout << "SQLite profile store tests passed\n";
    return 0;
}
