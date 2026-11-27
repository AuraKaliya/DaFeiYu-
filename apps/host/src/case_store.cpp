#include "message_store_db.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
// 约束用户提供条件的编码大小与有效字符，确保能随证据一起展示。
void bounded(const std::string& text,std::size_t bytes) {
    if (!runtime::valid_utf8(text) || text.find_first_not_of(" \t\r\n")==text.npos || Json(text).dump().size()>bytes)
        throw std::invalid_argument("comparison: invalid condition");
}
// 对象、度量、单位和条件一经绑定不可改写，重跑只接受原定义。
Json scope_for(sqlite3* db,const ComparisonTag& tag) {
    bounded(tag.checkpoint,66);
    Statement get(db,"SELECT scope FROM observation_case WHERE name=?"); get.bind(1,tag.name);
    if (!get.next()) throw std::invalid_argument("comparison: unknown case");
    return Json::parse(get.text(0));
}
}
// 按固定键保存不可变的比较定义，重复登记复用原范围。
void MessageStore::define_case(const std::string& name,const Json& scope) {
    if (name.empty() || name.size()>64 || name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-")!=name.npos ||
        scope.size()!=5 || (scope.at("criterion")!="literal" && scope.at("criterion")!="target"))
        throw std::invalid_argument("comparison: invalid name or criterion");
    for (const auto field : {"object","measure","unit","conditions"}) bounded(scope.at(field).get_ref<const std::string&>(),98);
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement add(db,"INSERT OR IGNORE INTO observation_case(name,scope) VALUES(?,?)"); add.bind(1,name); add.bind(2,scope.dump()); add.next();
    if (scope_for(db,{name,"check"})!=scope) throw std::invalid_argument("comparison: conflicting definition");
    index_case_terms(db,name,scope);
    tx.commit();
}
// 别名仅用于匹配；添加、删除均可重试，主题词数量独立于实际经历数。
void MessageStore::case_alias(const std::string& name,const std::string& alias,bool remove) {
    bounded(alias,98);
    if(alias.find('/')!=alias.npos || alias.find_first_of("\t\r\n")!=alias.npos || alias.front()==' ' || alias.back()==' ')
        throw std::invalid_argument("comparison: invalid alias");
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    scope_for(db,{name,"check"});
    Statement change(db,remove ? "DELETE FROM case_term WHERE name=? AND kind='alias' AND term=?" :
        "INSERT OR IGNORE INTO case_term(name,kind,term) VALUES(?,'alias',?)");
    change.bind(1,name); change.bind(2,alias); change.next();
    Statement n(db,"SELECT count(*) FROM case_term WHERE name=? AND kind='alias'"); n.bind(1,name); n.next();
    if(n.number(0)>8) throw std::invalid_argument("comparison: alias limit");
    tx.commit();
}
// 核对准备派发的任务引用的比较与采样检查点。
void MessageStore::validate_case(const ComparisonTag& tag) const {
    std::lock_guard lock(state_->mutex); scope_for(state_->db,tag);
}
// 只收已经核验的动作后读回或已送到的结束信。指纹用于全值比较，展示时仍取原件。
// 步骤：核实档案和原件 → 检查同号出处 → 提取可比值 → 按唯一引用入组。
void message_db::link_case_member(sqlite3* db,const std::string& profile,const std::string& name,const std::string& checkpoint,
    const std::string& reference,std::string_view attribution) {
    const auto scope=scope_for(db,{name,checkpoint}); const auto source=resolve_basis(db,profile,reference);
    if (!source || (source->kind!="tool_readback" && source->kind!="sender_report")) throw std::invalid_argument("comparison: unavailable observation");
    Statement prior(db,"SELECT name,checkpoint,active FROM case_member WHERE reference=?"); prior.bind(1,reference);
    if (prior.next()) {
        if (prior.text(0)!=name || prior.text(1)!=checkpoint || !prior.number(2)) throw std::invalid_argument("comparison: conflicting attribution");
        return;
    }
    std::string path,reader,reading; bool met=false;
    if (source->kind=="tool_readback") {
        Statement get(db,"SELECT family,expected,actual FROM task_observation WHERE task=?"); get.bind(1,source->record);
        if (!get.next()) throw std::invalid_argument("comparison: missing readback");
        path=get.text(0); reader=Json::parse(path).at(1); const auto actual=get.text(2);
        met=get.text(1)==actual; reading=scope.at("criterion")=="literal" ? platform::sha256(actual) : met ? "met" : "missed";
    }
    Statement add(db,"INSERT INTO case_member(name,checkpoint,reference,kind,record,dependency,attribution,path,reader,reading,met) VALUES(?,?,?,?,?,?,?,?,?,?,?)");
    add.bind(1,name); add.bind(2,checkpoint); add.bind(3,reference); add.bind(4,source->kind); add.bind(5,source->record);
    add.bind(6,source->dependency); add.bind(7,attribution); add.bind(8,path); add.bind(9,reader); add.bind(10,reading); add.bind(11,met); add.next();
}
// 在主人的事后归类下保存既有原件，同一次归类只计一次。
void MessageStore::attach_case(const ComparisonTag& tag,const std::string& reference) {
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    link_case_member(db,conversation(),tag.name,tag.checkpoint,reference,"owner_retrospective");
    sql(db,"UPDATE conversation SET memory_revision=1 WHERE memory_revision=0"); tx.commit();
}
// 撤销依赖原件的所有版本；尚未参与任何版本的新成员撤回时保留旧版本。
void message_db::revoke_case_basis(sqlite3* db,const std::string& dependency) {
    Statement invalid(db,"UPDATE case_opinion SET valid=0 WHERE EXISTS(SELECT 1 FROM case_member m WHERE m.active=1 AND m.dependency=? "
        "AND m.name=case_opinion.name AND m.id<=case_opinion.through_member)"); invalid.bind(1,dependency); invalid.next();
    Statement reset(db,"UPDATE observation_case SET head=CASE WHEN head IN (SELECT id FROM case_opinion WHERE valid=0) THEN 0 ELSE head END,cursor=0 "
        "WHERE EXISTS(SELECT 1 FROM case_member m WHERE m.active=1 AND m.dependency=? AND m.name=observation_case.name AND m.id<=observation_case.cursor)");
    reset.bind(1,dependency); reset.next();
    Statement hide(db,"UPDATE case_member SET active=0 WHERE dependency=? AND active=1"); hide.bind(1,dependency); hide.next();
}
}
