#pragma once
#include "message_store.hpp"
#include <winsqlite/winsqlite3.h>
#include <mutex>

namespace dfy::app::message_db {
// 在任务原件的写事务内索引独立核验过的观察，旧档案升级也复用同一规则。
void index_task_observation(sqlite3* db, std::int64_t id, const nlohmann::json& task);
// 将系统返回码映射为可公开核对的失败，避免泄漏 SQL 参数或消息正文。
inline void check(int code) {
    if (code!=SQLITE_OK) throw std::runtime_error("messages: SQLite error "+std::to_string(code));
}
// 执行宿主内置的固定 SQL。
inline void sql(sqlite3* db, const char* text) { check(sqlite3_exec(db,text,nullptr,nullptr,nullptr)); }

struct Transaction {
    sqlite3* db;
    // 在执行检查之前取得写入序列，确保并发重试看到已提交结果。
    explicit Transaction(sqlite3* connection) : db(connection) { sql(db,"BEGIN IMMEDIATE"); }
    // 失败或异常离开时撤销本次写入。
    ~Transaction() { if (db) sqlite3_exec(db,"ROLLBACK",nullptr,nullptr,nullptr); }
    // 写盘成功后转移到已完成状态，提交失败仍保留回滚责任。
    void commit() { sql(db,"COMMIT"); db = nullptr; }
};
struct Statement {
    sqlite3_stmt* stmt = nullptr;
    // 预编译一条固定语句，由本作用域独占游标。
    Statement(sqlite3* db, const char* text) { check(sqlite3_prepare_v2(db,text,-1,&stmt,nullptr)); }
    // 归还语句和它持有的读取位置。
    ~Statement() { sqlite3_finalize(stmt); }
    // 将带长度的 UTF-8 数据复制为 SQL 参数。
    void bind(int column, std::string_view value) {
        check(sqlite3_bind_text64(stmt,column,value.data(),value.size(),SQLITE_TRANSIENT,SQLITE_UTF8));
    }
    // 不经浮点格式转换绑定消息或修订号。
    void bind(int column, std::int64_t value) { check(sqlite3_bind_int64(stmt,column,value)); }
    // 读取下一行并区分正常结束与存储失败。
    bool next() {
        const int result = sqlite3_step(stmt);
        if (result!=SQLITE_ROW && result!=SQLITE_DONE) check(result);
        return result==SQLITE_ROW;
    }
    // 读取本行非空的整数列。
    std::int64_t number(int column) const { return sqlite3_column_int64(stmt,column); }
    // 在游标前进前复制数据库给出的 UTF-8 文本。
    std::string text(int column) const {
        const auto* value = sqlite3_column_text(stmt,column);
        if (!value) throw std::runtime_error("messages: missing stored text");
        return {reinterpret_cast<const char*>(value),static_cast<std::size_t>(sqlite3_column_bytes(stmt,column))};
    }
};
}

namespace dfy::app {
struct MessageStore::State {
    sqlite3* db = nullptr;
    std::mutex mutex;
    std::string conversation;
    // 关闭此实例打开的唯一数据库句柄。
    ~State() { if (db) sqlite3_close_v2(db); }
};
}
