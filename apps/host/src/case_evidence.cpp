#include "case_review.hpp"
#include <algorithm>

namespace dfy::app::case_review {
using namespace message_db;
using Json=nlohmann::json;
namespace {
constexpr char columns[]="SELECT id,record,reference,checkpoint,kind,path,reader,reading,attribution FROM case_member WHERE name=? AND active=1 AND id<=? ";
// 只将被索引选中的一份轻量成员复制到宿主。
Member member(const Statement& row) {
    return {row.number(0),row.number(1),row.text(2),row.text(3),row.text(4),row.text(5),row.text(6),row.text(7),row.text(8)};
}
// 同一原件只占一个优先位置。
void choose(Evidence& evidence,const std::optional<Member>& witness) {
    if(witness && std::ranges::none_of(evidence.members,[&](const auto& m){return m.id==witness->id;}))
        evidence.members.push_back(*witness);
}
}
// SQLite 聚合计数和检查点；宿主仅保留分歧双方、替代路径、不同读数和最近报告等至多六项。
// 步骤：计算范围内统计 → 找到存在独立分歧的检查点 → 选出一对确有分歧的见证 → 选择其余首屏来源。
Evidence classify(sqlite3* db,const std::string& name,const Json& scope,std::int64_t through) {
    Evidence out;
    Statement totals(db,"SELECT count(*),count(nullif(path,'')),count(DISTINCT nullif(path,'')),count(DISTINCT nullif(reading,'')),"
        "coalesce(max(met),0),coalesce(max(id),0) FROM case_member WHERE name=? AND active=1 AND id<=?");
    totals.bind(1,name); totals.bind(2,through); totals.next();
    out.total=static_cast<std::size_t>(totals.number(0)); out.natives=static_cast<std::size_t>(totals.number(1));
    out.paths=static_cast<std::size_t>(totals.number(2)); out.latest=totals.number(5);
    const auto readings=totals.number(3);
    const auto latest=[&](const char* condition,std::initializer_list<std::string_view> args={}) -> std::optional<Member> {
        const auto sql=std::string(columns)+condition+" ORDER BY id DESC LIMIT 1"; Statement q(db,sql.c_str());
        q.bind(1,name); q.bind(2,through); int i=3; for(const auto arg:args) q.bind(i++,arg);
        return q.next() ? std::optional(member(q)) : std::nullopt;
    };
    if(readings>1 && out.paths>1) {
        Statement clash(db,"SELECT checkpoint FROM case_member WHERE name=? AND active=1 AND kind='tool_readback' AND id<=? GROUP BY checkpoint "
            "HAVING min(reader)<>max(reader) AND min(reading)<>max(reading) ORDER BY max(id) DESC LIMIT 1");
        clash.bind(1,name); clash.bind(2,through);
        if(clash.next()) {
            const auto at=clash.text(0);
            auto a=latest("AND kind='tool_readback' AND checkpoint=?",{at});
            auto b=latest("AND kind='tool_readback' AND checkpoint=? AND reader<>? AND reading<>?",{at,a->reader,a->reading});
            if(!b) {
                // 最新一项与两方各共享一个属性时，另外两项才是能互相反驳的一对。
                b=latest("AND kind='tool_readback' AND checkpoint=? AND reading<>?",{at,a->reading});
                a=latest("AND kind='tool_readback' AND checkpoint=? AND reader<>?",{at,a->reader});
            }
            if(a->id<b->id) std::swap(a,b);
            out.conflict={a->ref,b->ref}; choose(out,a); choose(out,b);
        }
    }
    if(out.natives) {
        const auto first=latest("AND kind='tool_readback'"); choose(out,first);
        if(out.paths>1) choose(out,latest("AND kind='tool_readback' AND path<>?",{first->path}));
        if(readings>1) choose(out,latest("AND kind='tool_readback' AND reading<>?",{first->reading}));
    }
    if(out.total>out.natives) choose(out,latest("AND kind='sender_report'"));
    out.state=!out.total ? "no_evidence" : !out.natives ? "reports_only" : out.paths<2 ? "insufficient_paths" :
        !out.conflict.empty() ? "conflicting_observations" : scope.at("criterion")=="literal" ?
        (readings==1 ? "consistent_reading" : "variable_reading") : readings>1 ? "variable_result" :
        totals.number(4) ? "targets_met" : "targets_missed";
    return out;
}
// 首屏先放被选见证，其余从同一截止号按原成员倒序取当前页，不重建所有历史行。
std::vector<Member> page(sqlite3* db,const std::string& name,std::int64_t through,const Evidence& evidence,std::size_t offset,std::size_t count) {
    std::vector<Member> result;
    if(offset>=evidence.total || !count) return result;
    const auto& priority=evidence.members;
    for(auto i=offset;i<priority.size() && result.size()<count;++i) result.push_back(priority[i]);
    if(result.size()==count || offset+result.size()>=evidence.total) return result;
    const auto sql=std::string(columns)+" AND id NOT IN(?,?,?,?,?,?) ORDER BY id DESC LIMIT ? OFFSET ?";
    Statement rest(db,sql.c_str()); rest.bind(1,name); rest.bind(2,through);
    for(std::size_t i=0;i<6;++i) rest.bind(static_cast<int>(3+i),i<priority.size() ? priority[i].id : 0);
    rest.bind(9,static_cast<std::int64_t>(count-result.size()));
    rest.bind(10,static_cast<std::int64_t>(offset>priority.size() ? offset-priority.size() : 0));
    while(rest.next()) result.push_back(member(rest));
    return result;
}
}
