#include "storage/sqlite_profile_store.hpp"

#include "config/config_document.hpp"

#include "sqlite3.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace ccs {

namespace {

constexpr int kSqlTextLimit = 1024 * 1024;
constexpr int kSqlColumnLimit = 64;
constexpr int kSqlVariableLimit = 64;
constexpr int kSqlCompoundSelectLimit = 16;
constexpr int kDatabasePageSize = 4096;
constexpr int kDatabaseMaxPageCount = 65'536;
constexpr int kWalAutoCheckpointPages = 256;
constexpr std::int64_t kJournalSizeLimit = 16 * 1024 * 1024;

class StoreError final : public std::runtime_error {
public:
    StoreError(ProfileStoreFailure failure, std::string message)
        : std::runtime_error(std::move(message))
        , failure_(failure) {}

    ProfileStoreFailure failure() const noexcept {
        return failure_;
    }

private:
    ProfileStoreFailure failure_;
};

[[noreturn]] void fail(ProfileStoreFailure failure, std::string message) {
    throw StoreError(failure, std::move(message));
}

ProfileStoreFailure sqlite_failure(int result) {
    switch (result & 0xff) {
    case SQLITE_BUSY:
    case SQLITE_LOCKED:
        return ProfileStoreFailure::Busy;
    case SQLITE_CONSTRAINT:
        return ProfileStoreFailure::Constraint;
    case SQLITE_CORRUPT:
    case SQLITE_NOTADB:
        return ProfileStoreFailure::Corrupt;
    default:
        return ProfileStoreFailure::Io;
    }
}

std::string sqlite_error(sqlite3* database, std::string_view operation, int result) {
    std::string message(operation);
    message += " failed (SQLite ";
    message += std::to_string(result);
    message += "): ";
    message += database == nullptr ? sqlite3_errstr(result) : sqlite3_errmsg(database);
    return message;
}

void require_sqlite(sqlite3* database, int result, std::string_view operation) {
    if (result != SQLITE_OK) {
        fail(sqlite_failure(result), sqlite_error(database, operation, result));
    }
}

std::string path_to_utf8(const std::filesystem::path& path) {
    const auto value = path.u8string();
    return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}

class Connection final {
public:
    Connection(
        const std::filesystem::path& path,
        bool create,
        const SqliteProfileStoreOptions& options)
        : options_(options) {
        const auto encoded_path = path_to_utf8(path);
        int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
        if (create) {
            flags |= SQLITE_OPEN_CREATE;
        }
        sqlite3* opened = nullptr;
        const int result = sqlite3_open_v2(encoded_path.c_str(), &opened, flags, nullptr);
        if (result != SQLITE_OK) {
            const auto message = sqlite_error(opened, "open profile database", result);
            if (opened != nullptr) {
                sqlite3_close_v2(opened);
            }
            fail(sqlite_failure(result), message);
        }
        database_ = opened;

        require_sqlite(
            database_,
            sqlite3_extended_result_codes(database_, 1),
            "enable extended SQLite errors");
        require_sqlite(
            database_,
            sqlite3_busy_timeout(database_, options_.busy_timeout_ms),
            "configure SQLite busy timeout");
        sqlite3_limit(database_, SQLITE_LIMIT_LENGTH, static_cast<int>(kMaxStoredProfilePayloadBytes));
        sqlite3_limit(database_, SQLITE_LIMIT_SQL_LENGTH, kSqlTextLimit);
        sqlite3_limit(database_, SQLITE_LIMIT_COLUMN, kSqlColumnLimit);
        sqlite3_limit(database_, SQLITE_LIMIT_VARIABLE_NUMBER, kSqlVariableLimit);
        sqlite3_limit(database_, SQLITE_LIMIT_ATTACHED, 0);
        sqlite3_limit(database_, SQLITE_LIMIT_COMPOUND_SELECT, kSqlCompoundSelectLimit);
        sqlite3_limit(database_, SQLITE_LIMIT_WORKER_THREADS, 0);

        int defensive = 0;
        require_sqlite(
            database_,
            sqlite3_db_config(database_, SQLITE_DBCONFIG_DEFENSIVE, 1, &defensive),
            "enable SQLite defensive mode");
        if (defensive != 1) {
            fail(ProfileStoreFailure::Io, "SQLite defensive mode was not enabled");
        }
    }

    ~Connection() {
        if (database_ != nullptr) {
            sqlite3_close_v2(database_);
        }
    }

    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;

    sqlite3* get() const noexcept {
        return database_;
    }

    const SqliteProfileStoreOptions& options() const noexcept {
        return options_;
    }

private:
    sqlite3* database_ = nullptr;
    SqliteProfileStoreOptions options_;
};

class Statement final {
public:
    Statement(sqlite3* database, std::string_view sql)
        : database_(database) {
        if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            fail(ProfileStoreFailure::InvalidData, "SQL statement exceeds the supported size");
        }
        const int result = sqlite3_prepare_v3(
            database_,
            sql.data(),
            static_cast<int>(sql.size()),
            SQLITE_PREPARE_PERSISTENT,
            &statement_,
            nullptr);
        if (result != SQLITE_OK) {
            fail(sqlite_failure(result), sqlite_error(database_, "prepare SQL statement", result));
        }
    }

    ~Statement() {
        if (statement_ != nullptr) {
            sqlite3_finalize(statement_);
        }
    }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    int step() {
        const int result = sqlite3_step(statement_);
        if (result != SQLITE_ROW && result != SQLITE_DONE) {
            fail(sqlite_failure(result), sqlite_error(database_, "execute SQL statement", result));
        }
        return result;
    }

    void reset() {
        require_sqlite(database_, sqlite3_reset(statement_), "reset SQL statement");
        require_sqlite(database_, sqlite3_clear_bindings(statement_), "clear SQL bindings");
    }

    void bind_integer(int index, std::int64_t value) {
        require_sqlite(
            database_,
            sqlite3_bind_int64(statement_, index, value),
            "bind SQL integer");
    }

    void bind_text(int index, std::string_view value) {
        if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
            fail(ProfileStoreFailure::InvalidData, "SQL text value exceeds the supported size");
        }
        require_sqlite(
            database_,
            sqlite3_bind_text(
                statement_,
                index,
                value.data(),
                static_cast<int>(value.size()),
                SQLITE_TRANSIENT),
            "bind SQL text");
    }

    void bind_optional_text(int index, const std::optional<std::string>& value) {
        if (value) {
            bind_text(index, *value);
            return;
        }
        require_sqlite(database_, sqlite3_bind_null(statement_, index), "bind SQL null");
    }

    std::int64_t integer(int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_INTEGER) {
            fail(ProfileStoreFailure::Corrupt, "profile database integer column has the wrong type");
        }
        return sqlite3_column_int64(statement_, column);
    }

    std::string text(int column) const {
        if (sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
            fail(ProfileStoreFailure::Corrupt, "profile database text column has the wrong type");
        }
        const auto* value = sqlite3_column_text(statement_, column);
        const int bytes = sqlite3_column_bytes(statement_, column);
        if (value == nullptr || bytes < 0) {
            fail(ProfileStoreFailure::Corrupt, "profile database contains an invalid text value");
        }
        return std::string(reinterpret_cast<const char*>(value), static_cast<std::size_t>(bytes));
    }

    std::optional<std::string> optional_text(int column) const {
        if (sqlite3_column_type(statement_, column) == SQLITE_NULL) {
            return std::nullopt;
        }
        return text(column);
    }

private:
    sqlite3* database_ = nullptr;
    sqlite3_stmt* statement_ = nullptr;
};

void execute(sqlite3* database, std::string_view sql) {
    Statement statement(database, sql);
    if (statement.step() != SQLITE_DONE) {
        fail(ProfileStoreFailure::Io, "SQL statement unexpectedly returned rows");
    }
}

void drain(sqlite3* database, std::string_view sql) {
    Statement statement(database, sql);
    while (statement.step() == SQLITE_ROW) {
    }
}

std::int64_t scalar_integer(sqlite3* database, std::string_view sql) {
    Statement statement(database, sql);
    if (statement.step() != SQLITE_ROW) {
        fail(ProfileStoreFailure::Corrupt, "SQLite scalar query returned no row");
    }
    const auto result = statement.integer(0);
    if (statement.step() != SQLITE_DONE) {
        fail(ProfileStoreFailure::Corrupt, "SQLite scalar query returned multiple rows");
    }
    return result;
}

std::string scalar_text(sqlite3* database, std::string_view sql) {
    Statement statement(database, sql);
    if (statement.step() != SQLITE_ROW) {
        fail(ProfileStoreFailure::Corrupt, "SQLite scalar query returned no row");
    }
    auto result = statement.text(0);
    if (statement.step() != SQLITE_DONE) {
        fail(ProfileStoreFailure::Corrupt, "SQLite scalar query returned multiple rows");
    }
    return result;
}

class Transaction final {
public:
    explicit Transaction(sqlite3* database)
        : database_(database) {
        execute(database_, "BEGIN IMMEDIATE");
    }

    ~Transaction() {
        if (active_) {
            try {
                execute(database_, "ROLLBACK");
            } catch (...) {
            }
        }
    }

    void commit() {
        execute(database_, "COMMIT");
        active_ = false;
    }

private:
    sqlite3* database_ = nullptr;
    bool active_ = true;
};

void configure_connection(Connection& connection, bool creating) {
    auto* database = connection.get();
    if (creating) {
        drain(database, "PRAGMA page_size=4096");
    }
    if (scalar_integer(database, "PRAGMA page_size") != kDatabasePageSize) {
        fail(ProfileStoreFailure::UnsupportedSchema, "profile database page size must be 4096 bytes");
    }
    if (scalar_text(database, "PRAGMA journal_mode=WAL") != "wal") {
        fail(ProfileStoreFailure::Io, "profile database requires WAL journal mode");
    }

    drain(database, "PRAGMA foreign_keys=ON");
    drain(database, "PRAGMA synchronous=FULL");
    drain(database, "PRAGMA wal_autocheckpoint=256");
    drain(database, "PRAGMA journal_size_limit=16777216");
    drain(database, "PRAGMA mmap_size=0");
    drain(database, "PRAGMA trusted_schema=OFF");
    drain(database, "PRAGMA max_page_count=65536");

    if (scalar_integer(database, "PRAGMA foreign_keys") != 1
        || scalar_integer(database, "PRAGMA synchronous") != 2
        || scalar_integer(database, "PRAGMA wal_autocheckpoint") != kWalAutoCheckpointPages
        || scalar_integer(database, "PRAGMA journal_size_limit") != kJournalSizeLimit
        || scalar_integer(database, "PRAGMA mmap_size") != 0
        || scalar_integer(database, "PRAGMA trusted_schema") != 0
        || scalar_integer(database, "PRAGMA max_page_count") != kDatabaseMaxPageCount) {
        fail(ProfileStoreFailure::Io, "profile database PRAGMA configuration did not take effect");
    }
}

void create_schema(sqlite3* database) {
    execute(
        database,
        "CREATE TABLE repository_meta ("
        "singleton INTEGER PRIMARY KEY CHECK (singleton = 1),"
        "schema_version INTEGER NOT NULL CHECK (schema_version >= 1),"
        "revision INTEGER NOT NULL CHECK (revision >= 0),"
        "migrated_from_sha256 TEXT)");
    execute(
        database,
        "CREATE TABLE schema_history ("
        "migration_id INTEGER PRIMARY KEY,"
        "schema_version INTEGER NOT NULL,"
        "applied_at TEXT NOT NULL)");
    execute(
        database,
        "CREATE TABLE migration_history ("
        "migration_key INTEGER PRIMARY KEY AUTOINCREMENT,"
        "source_schema TEXT NOT NULL,"
        "source_sha256 TEXT NOT NULL,"
        "completed_at TEXT NOT NULL,"
        "UNIQUE (source_schema, source_sha256))");
    execute(
        database,
        "CREATE TABLE profiles ("
        "profile_key INTEGER PRIMARY KEY AUTOINCREMENT,"
        "profile_id TEXT NOT NULL COLLATE BINARY UNIQUE,"
        "position INTEGER NOT NULL UNIQUE CHECK (position >= 0),"
        "enabled INTEGER NOT NULL CHECK (enabled IN (0, 1)),"
        "protocol TEXT,"
        "local_request_path TEXT,"
        "local_usage_path TEXT,"
        "upstream_base_url TEXT,"
        "upstream_request_path TEXT,"
        "upstream_usage_path TEXT)");
    execute(
        database,
        "CREATE TABLE rules ("
        "rule_key INTEGER PRIMARY KEY AUTOINCREMENT,"
        "profile_key INTEGER NOT NULL REFERENCES profiles(profile_key) ON DELETE CASCADE,"
        "rule_id TEXT NOT NULL COLLATE BINARY,"
        "position INTEGER NOT NULL CHECK (position >= 0),"
        "enabled INTEGER NOT NULL CHECK (enabled IN (0, 1)),"
        "type TEXT NOT NULL,"
        "options_json TEXT NOT NULL "
        "CHECK (json_valid(options_json) AND json_type(options_json) = 'object'),"
        "UNIQUE (profile_key, rule_id),"
        "UNIQUE (profile_key, position))");
    execute(
        database,
        "INSERT INTO repository_meta(singleton, schema_version, revision) VALUES (1, 1, 0)");
    execute(
        database,
        "INSERT INTO schema_history(migration_id, schema_version, applied_at) "
        "VALUES (1, 1, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))");
    drain(database, "PRAGMA user_version=1");
}

bool valid_sha256(const std::string& value) {
    return value.size() == 64
        && std::all_of(value.begin(), value.end(), [](unsigned char ch) {
               return std::isdigit(ch) != 0 || (ch >= 'a' && ch <= 'f');
           });
}

bool add_payload_size(std::size_t& total, std::size_t amount) {
    if (amount > kMaxStoredProfilePayloadBytes
        || total > kMaxStoredProfilePayloadBytes - amount) {
        return false;
    }
    total += amount;
    return true;
}

bool parse_rule_options(
    const StoredRule& stored,
    RuleDefinition& definition,
    std::string& error) {
    if (stored.options_json.size() > kMaxStoredRuleOptionsBytes) {
        error = "rule " + stored.rule_id + " options exceed the 1 MiB limit";
        return false;
    }
    auto options = nlohmann::json::parse(
        stored.options_json.begin(), stored.options_json.end(), nullptr, false, true);
    if (options.is_discarded() || !options.is_object()) {
        error = "rule " + stored.rule_id + " options_json must be a JSON object";
        return false;
    }
    if (options.dump() != stored.options_json) {
        error = "rule " + stored.rule_id + " options_json is not canonical JSON";
        return false;
    }

    definition.id.value = stored.rule_id;
    definition.enabled = stored.enabled;
    definition.type = stored.type;
    for (auto item = options.begin(); item != options.end(); ++item) {
        definition.options.emplace(item.key(), item.value());
    }
    return true;
}

bool validate_profiles(
    const std::vector<StoredProfile>& profiles,
    bool persisted,
    std::string& error) {
    error.clear();
    if (profiles.size() > kMaxConfigProfiles) {
        error = "profile store exceeds the maximum profile count";
        return false;
    }

    std::unordered_set<std::string> profile_ids;
    std::unordered_set<ProfileKey> profile_keys;
    std::unordered_set<RuleKey> rule_keys;
    std::size_t route_count = 0;
    std::size_t total_payload = 0;
    for (const auto& stored : profiles) {
        if ((persisted && stored.key <= 0) || (!persisted && stored.key < 0)) {
            error = "profile store contains an invalid profile key";
            return false;
        }
        if (stored.key > 0 && !profile_keys.emplace(stored.key).second) {
            error = "profile store contains a duplicate profile key";
            return false;
        }
        if (!profile_ids.emplace(stored.profile_id).second) {
            error = "profile store contains duplicate profile id: " + stored.profile_id;
            return false;
        }
        if (stored.rules.size() > kMaxRulesPerProfile) {
            error = "profile " + stored.profile_id + " exceeds the maximum rule count";
            return false;
        }

        ProfileDefinition profile;
        profile.enabled = stored.enabled;
        if (stored.protocol) {
            profile.protocol = ProtocolId{*stored.protocol};
        }
        profile.local.request_path = stored.local_request_path;
        profile.local.usage_path = stored.local_usage_path;
        profile.upstream.base_url = stored.upstream_base_url;
        profile.upstream.request_path = stored.upstream_request_path;
        profile.upstream.usage_path = stored.upstream_usage_path;

        route_count += stored.local_request_path ? 1 : 0;
        route_count += stored.local_usage_path ? 1 : 0;
        std::size_t profile_text_size = 64 + stored.profile_id.size();
        const std::optional<std::string>* fields[] = {
            &stored.protocol,
            &stored.local_request_path,
            &stored.local_usage_path,
            &stored.upstream_base_url,
            &stored.upstream_request_path,
            &stored.upstream_usage_path,
        };
        for (const auto* field : fields) {
            if (*field) {
                profile_text_size += (*field)->size();
            }
        }

        profile.rules.reserve(stored.rules.size());
        for (const auto& rule : stored.rules) {
            if ((persisted && rule.key <= 0) || (!persisted && rule.key < 0)) {
                error = "profile store contains an invalid rule key";
                return false;
            }
            if (rule.key > 0 && !rule_keys.emplace(rule.key).second) {
                error = "profile store contains a duplicate rule key";
                return false;
            }
            RuleDefinition definition;
            if (!parse_rule_options(rule, definition, error)) {
                error = "profile " + stored.profile_id + ": " + error;
                return false;
            }
            profile_text_size += 64 + rule.rule_id.size() + rule.type.size()
                + rule.options_json.size();
            if (profile_text_size > kMaxStoredProfileRulesTextBytes) {
                error = "profile " + stored.profile_id + " canonical rules exceed 4 MiB";
                return false;
            }
            profile.rules.push_back(std::move(definition));
        }

        if (!validate_profile_definition(
                stored.profile_id, profile, stored.enabled, error)) {
            return false;
        }
        if (!add_payload_size(total_payload, profile_text_size)) {
            error = "profile store logical payload exceeds 64 MiB";
            return false;
        }
    }
    if (route_count > kMaxConfigRoutes) {
        error = "profile store exceeds the maximum route count";
        return false;
    }
    return true;
}

bool bool_column(const Statement& statement, int column) {
    const auto value = statement.integer(column);
    if (value != 0 && value != 1) {
        fail(ProfileStoreFailure::Corrupt, "profile database boolean column is invalid");
    }
    return value == 1;
}

void verify_integrity(sqlite3* database) {
    if (scalar_text(database, "PRAGMA quick_check(1)") != "ok") {
        fail(ProfileStoreFailure::Corrupt, "profile database quick_check failed");
    }
    Statement foreign_keys(database, "PRAGMA foreign_key_check");
    if (foreign_keys.step() != SQLITE_DONE) {
        fail(ProfileStoreFailure::Corrupt, "profile database foreign_key_check failed");
    }
}

void verify_schema(sqlite3* database) {
    const auto table_count = scalar_integer(
        database,
        "SELECT count(*) FROM sqlite_schema WHERE type='table' AND name IN ("
        "'repository_meta','schema_history','migration_history','profiles','rules')");
    if (table_count != 5) {
        fail(ProfileStoreFailure::UnsupportedSchema, "profile database schema tables are missing");
    }
    if (scalar_integer(
            database,
            "SELECT count(*) FROM sqlite_schema WHERE name NOT LIKE 'sqlite_%'")
        != 5) {
        fail(ProfileStoreFailure::UnsupportedSchema, "profile database has unexpected schema objects");
    }
    if (scalar_integer(database, "PRAGMA user_version") != kProfileStoreSchemaVersion) {
        fail(ProfileStoreFailure::UnsupportedSchema, "unsupported profile database user_version");
    }
    if (scalar_integer(
            database,
            "SELECT count(*) FROM schema_history "
            "WHERE migration_id=1 AND schema_version=1")
        != 1) {
        fail(ProfileStoreFailure::UnsupportedSchema, "profile database schema history is invalid");
    }
}

ProfileStoreSnapshot read_snapshot(sqlite3* database) {
    verify_schema(database);

    ProfileStoreSnapshot snapshot;
    {
        Statement metadata(
            database,
            "SELECT schema_version, revision, migrated_from_sha256 "
            "FROM repository_meta WHERE singleton=1");
        if (metadata.step() != SQLITE_ROW) {
            fail(ProfileStoreFailure::Corrupt, "profile database metadata row is missing");
        }
        const auto schema_version = metadata.integer(0);
        if (schema_version != kProfileStoreSchemaVersion) {
            fail(ProfileStoreFailure::UnsupportedSchema, "unsupported profile database schema version");
        }
        snapshot.revision = metadata.integer(1);
        if (snapshot.revision < 0) {
            fail(ProfileStoreFailure::Corrupt, "profile database revision is negative");
        }
        snapshot.migrated_from_sha256 = metadata.optional_text(2);
        if (metadata.step() != SQLITE_DONE) {
            fail(ProfileStoreFailure::Corrupt, "profile database contains duplicate metadata rows");
        }
    }
    if (snapshot.migrated_from_sha256
        && !valid_sha256(*snapshot.migrated_from_sha256)) {
        fail(ProfileStoreFailure::Corrupt, "profile database migration hash is invalid");
    }

    std::unordered_map<ProfileKey, std::size_t> profile_indexes;
    {
        Statement profiles(
            database,
            "SELECT profile_key, profile_id, position, enabled, protocol, "
            "local_request_path, local_usage_path, upstream_base_url, "
            "upstream_request_path, upstream_usage_path "
            "FROM profiles ORDER BY position");
        std::int64_t expected_position = 0;
        while (profiles.step() == SQLITE_ROW) {
            StoredProfile profile;
            profile.key = profiles.integer(0);
            profile.profile_id = profiles.text(1);
            if (profiles.integer(2) != expected_position++) {
                fail(ProfileStoreFailure::Corrupt, "profile positions are not contiguous");
            }
            profile.enabled = bool_column(profiles, 3);
            profile.protocol = profiles.optional_text(4);
            profile.local_request_path = profiles.optional_text(5);
            profile.local_usage_path = profiles.optional_text(6);
            profile.upstream_base_url = profiles.optional_text(7);
            profile.upstream_request_path = profiles.optional_text(8);
            profile.upstream_usage_path = profiles.optional_text(9);
            if (!profile_indexes.emplace(profile.key, snapshot.profiles.size()).second) {
                fail(ProfileStoreFailure::Corrupt, "profile database contains duplicate profile keys");
            }
            snapshot.profiles.push_back(std::move(profile));
        }
    }

    std::vector<std::int64_t> expected_rule_positions(snapshot.profiles.size(), 0);
    {
        Statement rules(
            database,
            "SELECT r.rule_key, r.profile_key, r.rule_id, r.position, r.enabled, "
            "r.type, r.options_json FROM rules r JOIN profiles p "
            "ON p.profile_key=r.profile_key ORDER BY p.position, r.position");
        while (rules.step() == SQLITE_ROW) {
            const auto profile_key = rules.integer(1);
            const auto profile = profile_indexes.find(profile_key);
            if (profile == profile_indexes.end()) {
                fail(ProfileStoreFailure::Corrupt, "rule references a missing profile");
            }
            const auto profile_index = profile->second;
            if (rules.integer(3) != expected_rule_positions[profile_index]++) {
                fail(ProfileStoreFailure::Corrupt, "rule positions are not contiguous");
            }
            StoredRule rule;
            rule.key = rules.integer(0);
            rule.rule_id = rules.text(2);
            rule.enabled = bool_column(rules, 4);
            rule.type = rules.text(5);
            rule.options_json = rules.text(6);
            snapshot.profiles[profile_index].rules.push_back(std::move(rule));
        }
    }

    std::string validation_error;
    if (!validate_profiles(snapshot.profiles, true, validation_error)) {
        fail(
            ProfileStoreFailure::Corrupt,
            "profile database semantic validation failed: " + validation_error);
    }
    return snapshot;
}

void bind_profile(Statement& statement, const StoredProfile& profile, std::int64_t position) {
    statement.bind_text(1, profile.profile_id);
    statement.bind_integer(2, position);
    statement.bind_integer(3, profile.enabled ? 1 : 0);
    statement.bind_optional_text(4, profile.protocol);
    statement.bind_optional_text(5, profile.local_request_path);
    statement.bind_optional_text(6, profile.local_usage_path);
    statement.bind_optional_text(7, profile.upstream_base_url);
    statement.bind_optional_text(8, profile.upstream_request_path);
    statement.bind_optional_text(9, profile.upstream_usage_path);
}

void bind_rule(Statement& statement, const StoredRule& rule, std::int64_t position) {
    statement.bind_text(1, rule.rule_id);
    statement.bind_integer(2, position);
    statement.bind_integer(3, rule.enabled ? 1 : 0);
    statement.bind_text(4, rule.type);
    statement.bind_text(5, rule.options_json);
}

void require_changed_one(sqlite3* database, std::string_view operation) {
    if (sqlite3_changes64(database) != 1) {
        fail(ProfileStoreFailure::Stale, std::string(operation) + " did not match one row");
    }
}

void passive_checkpoint(sqlite3* database) noexcept {
    int log_frames = 0;
    int checkpointed_frames = 0;
    (void)sqlite3_wal_checkpoint_v2(
        database,
        nullptr,
        SQLITE_CHECKPOINT_PASSIVE,
        &log_frames,
        &checkpointed_frames);
}

bool existing_regular_file(const std::filesystem::path& path, std::string& error) {
    std::error_code ec;
    const bool exists = std::filesystem::exists(path, ec);
    if (ec) {
        error = "failed to inspect profile database: " + ec.message();
        fail(ProfileStoreFailure::Io, error);
    }
    if (!exists) {
        return false;
    }
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec) {
        error = "failed to inspect profile database: " + ec.message();
        fail(ProfileStoreFailure::Io, error);
    }
    if (!std::filesystem::is_regular_file(status)) {
        fail(ProfileStoreFailure::Io, "profile database path must be a regular file");
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        fail(ProfileStoreFailure::Io, "failed to read profile database size: " + ec.message());
    }
    if (size == 0) {
        fail(ProfileStoreFailure::Corrupt, "existing profile database is empty");
    }
    return true;
}

} // namespace

SqliteProfileStore::SqliteProfileStore(
    std::filesystem::path database_path,
    SqliteProfileStoreOptions options)
    : database_path_(std::move(database_path))
    , options_(options) {
    if (options_.busy_timeout_ms < 0) {
        options_.busy_timeout_ms = 0;
    }
}

bool SqliteProfileStore::open_or_create(
    ProfileStoreSnapshot& snapshot,
    std::string& error) {
    error.clear();
    last_failure_ = ProfileStoreFailure::None;
    try {
        if (existing_regular_file(database_path_, error)) {
            return load(snapshot, error);
        }
        Connection connection(database_path_, true, options_);
        configure_connection(connection, true);
        Transaction transaction(connection.get());
        create_schema(connection.get());
        transaction.commit();
        snapshot = read_snapshot(connection.get());
        passive_checkpoint(connection.get());
        return true;
    } catch (const StoreError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ProfileStoreFailure::Io;
        error = std::string("failed to create profile database: ") + exception.what();
    }
    return false;
}

bool SqliteProfileStore::load(ProfileStoreSnapshot& snapshot, std::string& error) {
    error.clear();
    last_failure_ = ProfileStoreFailure::None;
    try {
        if (!existing_regular_file(database_path_, error)) {
            fail(ProfileStoreFailure::NotFound, "profile database does not exist");
        }
        Connection connection(database_path_, false, options_);
        configure_connection(connection, false);
        if (options_.integrity_check_on_load) {
            verify_integrity(connection.get());
        }
        snapshot = read_snapshot(connection.get());
        return true;
    } catch (const StoreError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ProfileStoreFailure::Io;
        error = std::string("failed to load profile database: ") + exception.what();
    }
    return false;
}

bool SqliteProfileStore::save(
    const ProfileStoreSnapshot& desired,
    ProfileStoreSnapshot& committed,
    std::string& error) {
    error.clear();
    last_failure_ = ProfileStoreFailure::None;
    std::string validation_error;
    if (desired.revision < 0
        || !validate_profiles(desired.profiles, false, validation_error)
        || (desired.migrated_from_sha256
            && !valid_sha256(*desired.migrated_from_sha256))) {
        last_failure_ = ProfileStoreFailure::InvalidData;
        error = desired.revision < 0
            ? "profile store revision cannot be negative"
            : validation_error.empty() ? "profile store migration hash is invalid" : validation_error;
        return false;
    }

    try {
        if (!existing_regular_file(database_path_, error)) {
            fail(ProfileStoreFailure::NotFound, "profile database does not exist");
        }
        Connection connection(database_path_, false, options_);
        configure_connection(connection, false);
        if (options_.integrity_check_on_load) {
            verify_integrity(connection.get());
        }
        Transaction transaction(connection.get());
        const auto current = read_snapshot(connection.get());
        if (current.revision != desired.revision) {
            fail(ProfileStoreFailure::Stale, "profile database revision changed");
        }
        if (current.migrated_from_sha256 != desired.migrated_from_sha256) {
            fail(ProfileStoreFailure::InvalidData, "profile database migration origin is immutable");
        }
        if (current == desired) {
            transaction.commit();
            committed = current;
            return true;
        }
        if (desired.revision == std::numeric_limits<ProfileRevision>::max()) {
            fail(ProfileStoreFailure::Constraint, "profile database revision is exhausted");
        }

        std::unordered_map<ProfileKey, const StoredProfile*> existing_profiles;
        std::unordered_map<RuleKey, ProfileKey> existing_rules;
        for (const auto& profile : current.profiles) {
            existing_profiles.emplace(profile.key, &profile);
            for (const auto& rule : profile.rules) {
                existing_rules.emplace(rule.key, profile.key);
            }
        }
        std::unordered_set<ProfileKey> desired_profile_keys;
        std::unordered_set<RuleKey> desired_rule_keys;
        for (const auto& profile : desired.profiles) {
            if (profile.key > 0) {
                if (existing_profiles.count(profile.key) == 0) {
                    fail(ProfileStoreFailure::Stale, "profile key no longer exists");
                }
                desired_profile_keys.emplace(profile.key);
            }
            for (const auto& rule : profile.rules) {
                if (rule.key == 0) {
                    continue;
                }
                const auto existing_rule = existing_rules.find(rule.key);
                if (profile.key == 0
                    || existing_rule == existing_rules.end()
                    || existing_rule->second != profile.key) {
                    fail(ProfileStoreFailure::Stale, "rule key no longer belongs to the profile");
                }
                desired_rule_keys.emplace(rule.key);
            }
        }

        execute(
            connection.get(),
            "UPDATE rules SET rule_id='#ccs-temp-rule-' || rule_key, "
            "position=position+1000000");
        execute(
            connection.get(),
            "UPDATE profiles SET profile_id='#ccs-temp-profile-' || profile_key, "
            "position=position+1000000");

        Statement delete_rule(connection.get(), "DELETE FROM rules WHERE rule_key=?1");
        for (const auto& [rule_key, profile_key] : existing_rules) {
            (void)profile_key;
            if (desired_rule_keys.count(rule_key) != 0) {
                continue;
            }
            delete_rule.reset();
            delete_rule.bind_integer(1, rule_key);
            if (delete_rule.step() != SQLITE_DONE) {
                fail(ProfileStoreFailure::Io, "delete rule returned rows");
            }
        }

        Statement delete_profile(connection.get(), "DELETE FROM profiles WHERE profile_key=?1");
        for (const auto& [profile_key, profile] : existing_profiles) {
            (void)profile;
            if (desired_profile_keys.count(profile_key) != 0) {
                continue;
            }
            delete_profile.reset();
            delete_profile.bind_integer(1, profile_key);
            if (delete_profile.step() != SQLITE_DONE) {
                fail(ProfileStoreFailure::Io, "delete profile returned rows");
            }
        }

        auto written = desired;
        Statement insert_profile(
            connection.get(),
            "INSERT INTO profiles(profile_id, position, enabled, protocol, "
            "local_request_path, local_usage_path, upstream_base_url, "
            "upstream_request_path, upstream_usage_path) "
            "VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)");
        Statement update_profile(
            connection.get(),
            "UPDATE profiles SET profile_id=?1, position=?2, enabled=?3, protocol=?4, "
            "local_request_path=?5, local_usage_path=?6, upstream_base_url=?7, "
            "upstream_request_path=?8, upstream_usage_path=?9 WHERE profile_key=?10");
        for (std::size_t index = 0; index < written.profiles.size(); ++index) {
            auto& profile = written.profiles[index];
            if (profile.key == 0) {
                insert_profile.reset();
                bind_profile(insert_profile, profile, static_cast<std::int64_t>(index));
                if (insert_profile.step() != SQLITE_DONE) {
                    fail(ProfileStoreFailure::Io, "insert profile returned rows");
                }
                profile.key = sqlite3_last_insert_rowid(connection.get());
                if (profile.key <= 0) {
                    fail(ProfileStoreFailure::Io, "inserted profile has an invalid key");
                }
            } else {
                update_profile.reset();
                bind_profile(update_profile, profile, static_cast<std::int64_t>(index));
                update_profile.bind_integer(10, profile.key);
                if (update_profile.step() != SQLITE_DONE) {
                    fail(ProfileStoreFailure::Io, "update profile returned rows");
                }
                require_changed_one(connection.get(), "update profile");
            }
        }

        Statement insert_rule(
            connection.get(),
            "INSERT INTO rules(profile_key, rule_id, position, enabled, type, options_json) "
            "VALUES (?6,?1,?2,?3,?4,?5)");
        Statement update_rule(
            connection.get(),
            "UPDATE rules SET rule_id=?1, position=?2, enabled=?3, type=?4, "
            "options_json=?5 WHERE rule_key=?6 AND profile_key=?7");
        for (auto& profile : written.profiles) {
            for (std::size_t index = 0; index < profile.rules.size(); ++index) {
                auto& rule = profile.rules[index];
                if (rule.key == 0) {
                    insert_rule.reset();
                    bind_rule(insert_rule, rule, static_cast<std::int64_t>(index));
                    insert_rule.bind_integer(6, profile.key);
                    if (insert_rule.step() != SQLITE_DONE) {
                        fail(ProfileStoreFailure::Io, "insert rule returned rows");
                    }
                    rule.key = sqlite3_last_insert_rowid(connection.get());
                    if (rule.key <= 0) {
                        fail(ProfileStoreFailure::Io, "inserted rule has an invalid key");
                    }
                } else {
                    update_rule.reset();
                    bind_rule(update_rule, rule, static_cast<std::int64_t>(index));
                    update_rule.bind_integer(6, rule.key);
                    update_rule.bind_integer(7, profile.key);
                    if (update_rule.step() != SQLITE_DONE) {
                        fail(ProfileStoreFailure::Io, "update rule returned rows");
                    }
                    require_changed_one(connection.get(), "update rule");
                }
            }
        }

        Statement update_revision(
            connection.get(),
            "UPDATE repository_meta SET revision=revision+1 "
            "WHERE singleton=1 AND revision=?1");
        update_revision.bind_integer(1, desired.revision);
        if (update_revision.step() != SQLITE_DONE) {
            fail(ProfileStoreFailure::Io, "update revision returned rows");
        }
        require_changed_one(connection.get(), "update profile revision");

        written.revision = desired.revision + 1;
        const auto verified = read_snapshot(connection.get());
        if (verified != written) {
            fail(ProfileStoreFailure::Corrupt, "profile database round-trip verification failed");
        }
        transaction.commit();
        passive_checkpoint(connection.get());
        committed = verified;
        return true;
    } catch (const StoreError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ProfileStoreFailure::Io;
        error = std::string("failed to save profile database: ") + exception.what();
    }
    return false;
}

bool SqliteProfileStore::mark_migrated(
    std::string source_schema,
    std::string source_sha256,
    ProfileStoreSnapshot& committed,
    std::string& error) {
    error.clear();
    last_failure_ = ProfileStoreFailure::None;
    if (source_schema != "ccs-trans.config/v2" || !valid_sha256(source_sha256)) {
        last_failure_ = ProfileStoreFailure::InvalidData;
        error = "migration provenance requires v2 schema and lowercase SHA-256";
        return false;
    }
    try {
        if (!existing_regular_file(database_path_, error)) {
            fail(ProfileStoreFailure::NotFound, "profile database does not exist");
        }
        Connection connection(database_path_, false, options_);
        configure_connection(connection, false);
        if (options_.integrity_check_on_load) {
            verify_integrity(connection.get());
        }
        Transaction transaction(connection.get());
        const auto current = read_snapshot(connection.get());
        if (current.migrated_from_sha256) {
            if (*current.migrated_from_sha256 == source_sha256) {
                transaction.commit();
                committed = current;
                return true;
            }
            fail(ProfileStoreFailure::Constraint, "profile database has different migration provenance");
        }

        Statement metadata(
            connection.get(),
            "UPDATE repository_meta SET migrated_from_sha256=?1 "
            "WHERE singleton=1 AND migrated_from_sha256 IS NULL");
        metadata.bind_text(1, source_sha256);
        if (metadata.step() != SQLITE_DONE) {
            fail(ProfileStoreFailure::Io, "migration metadata update returned rows");
        }
        require_changed_one(connection.get(), "set migration provenance");

        Statement history(
            connection.get(),
            "INSERT INTO migration_history(source_schema, source_sha256, completed_at) "
            "VALUES (?1, ?2, strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))");
        history.bind_text(1, source_schema);
        history.bind_text(2, source_sha256);
        if (history.step() != SQLITE_DONE) {
            fail(ProfileStoreFailure::Io, "migration history insert returned rows");
        }

        const auto verified = read_snapshot(connection.get());
        if (verified.revision != current.revision
            || verified.migrated_from_sha256 != source_sha256
            || verified.profiles != current.profiles) {
            fail(ProfileStoreFailure::Corrupt, "migration provenance round-trip failed");
        }
        transaction.commit();
        passive_checkpoint(connection.get());
        committed = verified;
        return true;
    } catch (const StoreError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ProfileStoreFailure::Io;
        error = std::string("failed to mark profile migration: ") + exception.what();
    }
    return false;
}

bool SqliteProfileStore::checkpoint_for_move(std::string& error) {
    error.clear();
    last_failure_ = ProfileStoreFailure::None;
    try {
        if (!existing_regular_file(database_path_, error)) {
            fail(ProfileStoreFailure::NotFound, "profile database does not exist");
        }
        {
            Connection connection(database_path_, false, options_);
            configure_connection(connection, false);
            verify_integrity(connection.get());
            (void)read_snapshot(connection.get());
            int log_frames = 0;
            int checkpointed_frames = 0;
            const int result = sqlite3_wal_checkpoint_v2(
                connection.get(),
                nullptr,
                SQLITE_CHECKPOINT_TRUNCATE,
                &log_frames,
                &checkpointed_frames);
            if (result != SQLITE_OK) {
                fail(
                    sqlite_failure(result),
                    sqlite_error(connection.get(), "truncate profile WAL", result));
            }
            if (log_frames != 0 || checkpointed_frames != 0) {
                fail(ProfileStoreFailure::Busy, "profile WAL did not truncate completely");
            }
        }

        for (const auto& suffix : {"-wal", "-shm"}) {
            auto sidecar = database_path_;
            sidecar += suffix;
            std::error_code ec;
            if (!std::filesystem::exists(sidecar, ec)) {
                if (ec) {
                    fail(ProfileStoreFailure::Io, "failed to inspect SQLite sidecar: " + ec.message());
                }
                continue;
            }
            const auto size = std::filesystem::file_size(sidecar, ec);
            if (ec || size != 0) {
                fail(
                    ProfileStoreFailure::Busy,
                    "SQLite sidecar remains after truncate checkpoint: " + sidecar.string());
            }
        }
        return true;
    } catch (const StoreError& exception) {
        last_failure_ = exception.failure();
        error = exception.what();
    } catch (const std::exception& exception) {
        last_failure_ = ProfileStoreFailure::Io;
        error = std::string("failed to checkpoint profile database: ") + exception.what();
    }
    return false;
}

bool SqliteProfileStore::verify(std::string& error) {
    ProfileStoreSnapshot snapshot;
    return load(snapshot, error);
}

const std::filesystem::path& SqliteProfileStore::path() const noexcept {
    return database_path_;
}

ProfileStoreFailure SqliteProfileStore::last_failure() const noexcept {
    return last_failure_;
}

} // namespace ccs
