#include <dafeiyu/sqlite.hpp>
#include <limits>
#include <stdexcept>

namespace dfy {
// 预编译 SQL 语句，并报告数据库返回的准备错误。
Statement::Statement(sqlite3* db, std::string_view sql) {
    const std::string owned(sql);
    if (sqlite3_prepare_v2(db, owned.c_str(), -1, &stmt_, nullptr) != SQLITE_OK)
        throw std::runtime_error(std::string("SQL_PREPARE: ") + sqlite3_errmsg(db));
}

// 释放预编译语句句柄。
Statement::~Statement() {
    sqlite3_finalize(stmt_);
}

// 复制并绑定文本参数，拒绝超长值或绑定失败。
Statement& Statement::bind(int index, std::string_view value) {
    if (value.size() > static_cast<size_t>(std::numeric_limits<int>::max()) ||
        sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) != SQLITE_OK)
        throw std::runtime_error("SQL_BIND");
    return *this;
}

// 推进 SQL 执行，区分返回一行、执行结束及数据库错误。
bool Statement::step() {
    const auto rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW)
        return true;

    if (rc == SQLITE_DONE)
        return false;
    throw std::runtime_error(std::string("SQL_STEP: ") + sqlite3_errmsg(sqlite3_db_handle(stmt_)));
}

// 读取结果列的完整文本，空指针列返回空字符串。
std::string Statement::column(int index) const {
    const auto* value = sqlite3_column_text(stmt_, index);
    return value ? std::string(
                       reinterpret_cast<const char*>(value), static_cast<size_t>(sqlite3_column_bytes(stmt_, index)))
                 : std::string{};
}

// 打开 SQLite 数据库并配置一致性与并发访问选项。
// 步骤：创建父目录并转换路径 → 打开数据库 → 设置忙等待、外键、WAL 和同步模式 → 配置失败时释放句柄。
Database::Database(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());

    const auto utf8 = path.u8string();
    if (sqlite3_open_v2(reinterpret_cast<const char*>(utf8.c_str()), &db_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("SQL_OPEN");
    }

    try {
        sqlite3_busy_timeout(db_, 5000);
        exec("PRAGMA foreign_keys=ON; PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL;");
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

// 关闭持有的 SQLite 数据库连接。
Database::~Database() {
    if (db_)
        sqlite3_close(db_);
}

// 执行无需逐行读取的 SQL，并在失败时释放错误文本后抛出异常。
void Database::exec(std::string_view sql) {
    char* error = nullptr;
    const std::string owned(sql);

    if (sqlite3_exec(db_, owned.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
        const std::string message = error ? error : "SQL_EXEC";
        sqlite3_free(error);
        throw std::runtime_error(message);
    }
}

// 校验或初始化数据库的实例、组件和模式版本身份。
// 步骤：开启事务并确保身份表存在 → 比较已有身份或写入初始身份 → 提交事务。
void Database::verify_identity(std::string_view instance, std::string_view component, int schema_version) {
    Transaction tx(*this);
    exec(
        "CREATE TABLE IF NOT EXISTS identity (id INTEGER PRIMARY KEY CHECK(id=1), instance TEXT NOT NULL, component TEXT NOT NULL, version INTEGER NOT NULL)");
    {
        auto read = prepare("SELECT instance, component, version FROM identity WHERE id=1");
        if (read.step()) {
            if (read.column(0) != instance || read.column(1) != component ||
                read.column(2) != std::to_string(schema_version))
                throw std::runtime_error("STORAGE_IDENTITY_OR_VERSION_MISMATCH");
        } else {
            auto insert = prepare("INSERT INTO identity VALUES(1,?,?,?)");
            insert.bind(1, instance).bind(2, component).bind(3, std::to_string(schema_version)).step();
        }
    }

    tx.commit();
}

// 在事务尚未提交时尝试回滚，抑制析构过程中的异常。
Transaction::~Transaction() {
    if (!committed_) {
        try {
            db_.exec("ROLLBACK");
        } catch (...) {
        }
    }
}
}
