#pragma once
#include <sqlite3.h>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace dfy {
class Statement {
    sqlite3_stmt* stmt_{};

public:
    // 预编译 SQL 语句，并报告数据库返回的准备错误。
    Statement(sqlite3* db, std::string_view sql);
    // 释放预编译语句句柄。
    ~Statement();
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    Statement(const Statement&) = delete;
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    Statement& operator=(const Statement&) = delete;
    // 复制并绑定文本参数，拒绝超长值或绑定失败。
    Statement& bind(int index, std::string_view value);
    // 推进 SQL 执行，区分返回一行、执行结束及数据库错误。
    bool step();
    // 读取结果列的完整文本，空指针列返回空字符串。
    std::string column(int index) const;

    // 读取语句执行产生的全表扫描步数。
    int fullscan_steps() const { return sqlite3_stmt_status(stmt_, SQLITE_STMTSTATUS_FULLSCAN_STEP, 0); }

    // 读取语句执行产生的 SQLite 虚拟机步数。
    int vm_steps() const { return sqlite3_stmt_status(stmt_, SQLITE_STMTSTATUS_VM_STEP, 0); }
};

class Database {
    sqlite3* db_{};

public:
    // 打开 SQLite 数据库并配置一致性与并发访问选项。
    explicit Database(const std::filesystem::path& path);
    // 关闭持有的 SQLite 数据库连接。
    ~Database();
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    Database(const Database&) = delete;
    // 禁止复制或赋值，保持当前对象独占的资源与状态所有权。
    Database& operator=(const Database&) = delete;
    // 执行无需逐行读取的 SQL，并在失败时释放错误文本后抛出异常。
    void exec(std::string_view sql);

    // 为当前连接创建预编译 SQL 语句。
    Statement prepare(std::string_view sql) { return Statement(db_, sql); }

    // 校验或初始化数据库的实例、组件和模式版本身份。
    void verify_identity(std::string_view instance, std::string_view component, int schema_version = 2);
};

class Transaction {
    Database& db_;
    bool committed_{};

public:
    // 开启立即事务并保存所属数据库连接。
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }

    // 在事务尚未提交时尝试回滚，抑制析构过程中的异常。
    ~Transaction();

    // 提交事务，并标记析构时无需回滚。
    void commit() {
        db_.exec("COMMIT");
        committed_ = true;
    }
};
}
