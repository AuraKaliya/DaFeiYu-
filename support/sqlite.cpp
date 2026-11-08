#include <dafeiyu/sqlite.hpp>
#include <limits>
#include <stdexcept>

namespace dfy {
Statement::Statement(sqlite3* db, std::string_view sql) {
    const std::string owned(sql);
    if (sqlite3_prepare_v2(db, owned.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("SQL_PREPARE: ") + sqlite3_errmsg(db));
}
Statement::~Statement() { sqlite3_finalize(stmt_); }
Statement& Statement::bind(int index, std::string_view value) {
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("SQL_BIND");
    return *this;
}
bool Statement::step() {
    const auto rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    throw std::runtime_error(std::string("SQL_STEP: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_)));
}
std::string Statement::column(int index) const {
    const auto* value = sqlite3_column_text(stmt_, index);
    return value ? std::string(reinterpret_cast<const char*>(value), static_cast<size_t>(sqlite3_column_bytes(stmt_, index))) : std::string{};
}
Database::Database(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    const auto utf8 = path.u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (db_) sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQL_OPEN");
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=DELETE; PRAGMA synchronous=FULL;");
    } catch (...) { sqlite3_close(db_); db_ = nullptr; throw; }
}
Database::~Database() { if (db_) sqlite3_close(db_); }
void Database::exec(std::string_view sql) {
    char* error = nullptr;
    const std::string owned(sql);
    if (sqlite3_exec(db_, owned.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : "SQL_EXEC";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}
void Database::verify_identity(std::string_view instance, std::string_view component) {
    Transaction tx(*this);
    exec("CREATE TABLE IF NOT EXISTS identity (id INTEGER PRIMARY KEY CHECK(id=1), instance TEXT NOT NULL, component TEXT NOT NULL, version INTEGER NOT NULL)");
    {
        auto read = prepare("SELECT instance, component, version FROM identity WHERE id=1");
        if (read.step()) {
            if (read.column(0) != instance || read.column(1) != component || read.column(2) != "1") throw std::runtime_error("STORAGE_IDENTITY_OR_VERSION_MISMATCH");
        } else {
            auto insert = prepare("INSERT INTO identity VALUES(1,?,?,1)");
            insert.bind(1, instance).bind(2, component).step();
        }
    }
    tx.commit();
}
Transaction::~Transaction() { if (!committed_) { try { db_.exec("ROLLBACK"); } catch (...) {} } }
}
