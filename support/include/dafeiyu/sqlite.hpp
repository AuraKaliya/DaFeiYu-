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
    Statement(sqlite3* db, std::string_view sql);
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& bind(int index, std::string_view value);
    bool step();
    std::string column(int index) const;
};
class Database {
    sqlite3* db_{};
public:
    explicit Database(const std::filesystem::path& path);
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    void exec(std::string_view sql);
    Statement prepare(std::string_view sql) { return Statement(db_, sql); }
    void verify_identity(std::string_view instance, std::string_view component, int schema_version = 2);
};
class Transaction {
    Database& db_;
    bool committed_{};
public:
    explicit Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE"); }
    ~Transaction();
    void commit() { db_.exec("COMMIT"); committed_ = true; }
};
}
