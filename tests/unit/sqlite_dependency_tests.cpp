#include "sqlite3.h"

#include <cstdlib>
#include <cstring>
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

class Database final {
public:
    ~Database() {
        if (handle_ != nullptr) {
            sqlite3_close(handle_);
        }
    }

    sqlite3** output() noexcept {
        return &handle_;
    }

    sqlite3* get() const noexcept {
        return handle_;
    }

private:
    sqlite3* handle_ = nullptr;
};

int scalar_int(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) == SQLITE_OK,
            sqlite3_errmsg(database));

    const int step_result = sqlite3_step(statement);
    require(step_result == SQLITE_ROW, sqlite3_errmsg(database));
    const int result = sqlite3_column_int(statement, 0);
    require(sqlite3_finalize(statement) == SQLITE_OK, sqlite3_errmsg(database));
    return result;
}

void require_exec(sqlite3* database, const char* sql) {
    char* message = nullptr;
    const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
    std::string error = message == nullptr ? sqlite3_errmsg(database) : message;
    sqlite3_free(message);
    require(result == SQLITE_OK, error);
}

} // namespace

int main() {
    require(std::strcmp(sqlite3_libversion(), "3.53.3") == 0,
            "unexpected SQLite library version");
    require(sqlite3_libversion_number() == 3053003,
            "unexpected SQLite numeric version");
    require(std::strcmp(
                sqlite3_sourceid(),
                "2026-06-26 20:14:12 "
                "d4c0e51e4aeb96955b99185ab9cde75c339e2c29c3f3f12428d364a10d782c62")
                == 0,
            "unexpected SQLite source id");
    require(sqlite3_threadsafe() == 1, "SQLite serialized threading is disabled");

    constexpr const char* required_options[] = {
        "THREADSAFE=1",
        "DQS=0",
        "DEFAULT_FOREIGN_KEYS",
        "ENABLE_API_ARMOR",
        "LIKE_DOESNT_MATCH_BLOBS",
        "OMIT_DEPRECATED",
        "OMIT_LOAD_EXTENSION",
    };
    for (const char* option : required_options) {
        require(sqlite3_compileoption_used(option) != 0,
                std::string("missing SQLite compile option: ") + option);
    }

    Database database;
    require(sqlite3_open_v2(
                ":memory:",
                database.output(),
                SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                nullptr)
                == SQLITE_OK,
            "unable to open in-memory SQLite database");
    require(scalar_int(database.get(), "PRAGMA foreign_keys") == 1,
            "foreign keys are not enabled by default");
    require(scalar_int(database.get(), "SELECT json_valid('{\"ok\":true}')") == 1,
            "SQLite JSON functions are unavailable");

    require_exec(database.get(), "CREATE TABLE parent(id INTEGER PRIMARY KEY)");
    require_exec(
        database.get(),
        "CREATE TABLE child(parent_id INTEGER REFERENCES parent(id))");
    const int foreign_key_result = sqlite3_exec(
        database.get(), "INSERT INTO child(parent_id) VALUES (7)", nullptr, nullptr, nullptr);
    require((foreign_key_result & 0xff) == SQLITE_CONSTRAINT,
            "foreign key constraint was not enforced");

    sqlite3_stmt* statement = nullptr;
    require(sqlite3_prepare_v2(
                database.get(), "SELECT \"not_a_column\"", -1, &statement, nullptr)
                == SQLITE_ERROR,
            "double-quoted string literals remain enabled");
    require(statement == nullptr, "failed prepare returned a statement");
    require(sqlite3_prepare_v2(nullptr, "SELECT 1", -1, &statement, nullptr) == SQLITE_MISUSE,
            "SQLite API armor is unavailable");

    std::cout << "SQLite dependency probe passed\n";
    return 0;
}
