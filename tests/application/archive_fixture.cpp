#include "archive_fixture.hpp"
#include "task_memory_fixture.hpp"
#include "message_store_db.hpp"

namespace dfy::tests {
using namespace app::message_db;
namespace fs=std::filesystem;
namespace {
// 给一处独立测试档案建立 SQLite 连接。
auto connection(const fs::path& dir) {
    sqlite3* handle=nullptr; const auto file=(dir/"main-conversation.sqlite3").u8string();
    check(sqlite3_open(reinterpret_cast<const char*>(file.c_str()),&handle));
    return std::unique_ptr<sqlite3,decltype(&sqlite3_close)>(handle,&sqlite3_close);
}
// 读取失败前后的版本、身份行与主题索引存在性，确认尝试升级没有残留写入。
auto signature(sqlite3* db) {
    Statement version(db,"PRAGMA user_version"); version.next();
    Statement identities(db,"SELECT coalesce(group_concat(id||'/'||memory_revision,','),'') FROM conversation ORDER BY rowid"); identities.next();
    Statement terms(db,"SELECT count(*) FROM sqlite_master WHERE type='table' AND name='case_term'"); terms.next();
    return std::tuple{version.number(0),identities.text(0),terms.number(0)};
}
}
// 删除测试档案中晚于该版本出现的结构，原版本真实写入的业务行逐字保留。
void legacy_archive(const fs::path& directory,unsigned version) {
    task_memory::require(version>=9 && version<=13,"unsupported fixture schema");
    auto conn=connection(directory); auto* db=conn.get(); Transaction tx(db);
    sql(db,"DROP VIEW IF EXISTS readable_letter; DROP TABLE IF EXISTS idle_schedule; DROP INDEX IF EXISTS case_native_checkpoint; "
        "DROP INDEX IF EXISTS case_kind_sequence; DROP TABLE case_term; DROP INDEX case_checkpoint");
    if(version<13) sql(db,"DROP TABLE letter_digest");
    if(version<12) sql(db,"DROP TABLE case_opinion; DROP TABLE case_member; DROP TABLE observation_case");
    if(version<11) sql(db,"DROP TABLE knowledge_grant; DROP TABLE knowledge_basis; DROP TABLE knowledge_edition; DROP TABLE knowledge_article");
    if(version<10) sql(db,"DROP TABLE child_letter");
    sql(db,("PRAGMA user_version="+std::to_string(version)).c_str()); tx.commit();
}
// 旧版可能按新的授权写结束信，开始时的原件仍留在启动信中。
void legacy_letter_scope(const fs::path& directory,const std::string& letter,const std::string& source) {
    auto conn=connection(directory); auto* db=conn.get(); Transaction tx(db);
    const auto id=std::stoll(letter.substr(letter.rfind('/')+1)),src=std::stoll(source.substr(source.rfind('/')+1));
    Statement change(db,"UPDATE child_letter SET source=? WHERE id=? AND phase='end'"); change.bind(1,src); change.bind(2,id); change.next();
    task_memory::require(sqlite3_changes(db)==1,"missing legacy mail");
    Statement affected(db,"UPDATE case_member SET dependency=? WHERE reference=?"); affected.bind(1,source); affected.bind(2,letter); affected.next();
    Statement basis(db,"UPDATE knowledge_basis SET dependency=? WHERE reference=?"); basis.bind(1,source); basis.bind(2,letter); basis.next();
    sql(db,"DROP VIEW IF EXISTS readable_letter; PRAGMA user_version=16"); tx.commit();
}
// 全部异常样本建在测试自有目录，检查失败重开前后数据与版本相等。
void check_unusable_archive(const fs::path& temporary,const std::string& fault) {
    const auto dir=temporary/("archive-check-"+fault);
    fs::create_directories(dir); app::MessageStore{dir}; legacy_archive(dir,13);
    auto conn=connection(dir);
    if(fault=="multiple") sql(conn.get(),"INSERT INTO conversation SELECT lower(hex(randomblob(16))),memory_revision FROM conversation");
    else if(fault=="malformed") sql(conn.get(),"UPDATE conversation SET id='incorrect-identity'");
    else if(fault=="empty") sql(conn.get(),"DELETE FROM conversation");
    else if(fault=="future") sql(conn.get(),"PRAGMA user_version=999");
    else throw std::runtime_error("unknown archive fault");
    const auto before=signature(conn.get()); conn.reset(); bool refused=false;
    try {app::MessageStore opened(dir);} catch(const std::runtime_error&) {refused=true;}
    task_memory::require(refused,"unusable archive admitted: "+fault);
    conn=connection(dir); task_memory::require(signature(conn.get())==before,"failed open mutated archive");
}
}
