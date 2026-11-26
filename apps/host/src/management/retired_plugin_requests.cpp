#include "retired_plugin_requests.hpp"
#include <winsqlite/winsqlite3.h>
#include <memory>
#include <stdexcept>

namespace dfy::management {
namespace {
// 将本次登记或查询的数据库错误定位到插件管理回执，不公开参数。
void checked(int status) {
    if (status!=SQLITE_OK) throw std::runtime_error("plugins: temporary receipt storage unavailable");
}
// 在宿主互斥范围内执行一次按公开提交号绑定的插入或查询。
bool request_row(sqlite3* db,const char* query,const std::string& submission) {
    sqlite3_stmt* raw=nullptr;
    checked(sqlite3_prepare_v2(db,query,-1,&raw,nullptr));
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> statement(raw,&sqlite3_finalize);
    checked(sqlite3_bind_text(raw,1,submission.data(),static_cast<int>(submission.size()),SQLITE_STATIC));
    const auto result=sqlite3_step(raw);
    if (result!=SQLITE_ROW && result!=SQLITE_DONE) checked(result);
    return result==SQLITE_ROW;
}
}

// 关闭匿名临时数据库，操作系统随连接回收当前后台的退休号。
RetiredPluginRequests::~RetiredPluginRequests() { if (db_) sqlite3_close(db_); }

// 延迟建立仅保存公开提交号的临时索引，失去详细回执后仍能拒绝再次执行。
void RetiredPluginRequests::remember(const std::string& submission) {
    if (!db_) {
        sqlite3* raw=nullptr;
        const auto status=sqlite3_open_v2("",&raw,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_NOMUTEX,nullptr);
        std::unique_ptr<sqlite3,decltype(&sqlite3_close)> pending(raw,&sqlite3_close);
        checked(status);
        checked(sqlite3_exec(raw,"CREATE TABLE retired(submission TEXT PRIMARY KEY) WITHOUT ROWID",nullptr,nullptr,nullptr));
        db_=pending.release();
    }
    (void)request_row(db_,"INSERT OR IGNORE INTO retired VALUES(?)",submission);
}

// 读取已存在的临时索引，首次退休前直接确认记录不存在。
bool RetiredPluginRequests::contains(const std::string& submission) const {
    return db_ && request_row(db_,"SELECT 1 FROM retired WHERE submission=?",submission);
}
}
