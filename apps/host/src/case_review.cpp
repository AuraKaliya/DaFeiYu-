#include "case_review.hpp"
#include <algorithm>
#include <charconv>
#include <set>

namespace dfy::app {
using namespace message_db;
using namespace case_review;
using Json=nlohmann::json;
namespace {
// 展示内容限制按实际 JSON 编码计算；原始值仍可按回执号继续查看。
std::string preview(const std::string& value) {
    if (Json(value).dump().size()<=74) return value;
    auto end=std::min<std::size_t>(69,value.size());
    for (;;) {
        while (end && end<value.size() && (static_cast<unsigned char>(value[end])&0xc0)==0x80) --end;
        const auto first=value.substr(0,end);
        if (Json(first).dump().size()<=71) return first+"…";
        end/=2;
    }
}
// 将现场证据分歧映射为可写入的保留意见。
std::string judgement(const std::string& state) {return state=="conflicting_observations" ? "unresolved" : state;}
// 只载入仍可引用的本体解释及其原件编号。
Json opinion(sqlite3* db,const std::string& profile,std::int64_t id) {
    Statement row(db,"SELECT parent,through_member,judgement,body,cites FROM case_opinion WHERE valid=1 AND id=?"); row.bind(1,id);
    if (!row.next()) return nullptr;
    const auto prefix=profile+"/comparison-opinion/";
    Json out{{"author","main"},{"reference",prefix+std::to_string(id)},{"judgement",row.text(2)},{"text",row.text(3)},{"cites",Json::parse(row.text(4))}};
    if (row.number(0)) out["previousReference"]=prefix+std::to_string(row.number(0));
    return out;
}
// 冻结一次空闲复盘的条件、版本、见证及数量，提交时逐项复核。
Json batch_for(sqlite3* db,const std::string& profile,const std::string& name,const Json& scope,std::int64_t cursor,std::int64_t head,std::int64_t through) {
    const auto evidence=classify(db,name,scope,through);
    Json result{{"case",name},{"scope",scope},{"cursor",cursor},{"head",head},{"through",through},{"prior",opinion(db,profile,head)},
        {"state",evidence.state},{"nativeCount",evidence.natives},{"reportCount",evidence.total-evidence.natives},
        {"pathCount",evidence.paths},{"facts",Json::array()},{"requiredConflict",Json::array()}};
    result["requiredConflict"]=evidence.conflict;
    for (const auto& witness:case_review::page(db,name,through,evidence,0,4)) result["facts"].push_back(fact(db,witness));
    return result;
}
}


// 展开主 Agent 的原动作与读回或带作者的结束信。
Json case_review::fact(sqlite3* db,const Member& row) {
    Json item{{"reference",row.ref},{"checkpoint",row.checkpoint},{"attribution",row.attribution}};
    if (row.kind=="sender_report") {
        Statement get(db,"SELECT envelope FROM child_letter WHERE id=?"); get.bind(1,row.record); get.next();
        const auto env=Json::parse(get.text(0));
        item["origin"]="sender_report"; item["author"]=env.at("author"); item["exit"]=env.at("outcome");
        if(env.contains("summary")) {item["report"]=preview(env.at("summary")); item["summaryKind"]=env.at("summaryKind");}
    } else {
        Statement get(db,"SELECT expected,actual,action,effect FROM task_observation WHERE task=?"); get.bind(1,row.record); get.next();
        const auto base=row.ref.substr(0,row.ref.rfind("/receipt/")), seq=std::to_string(get.number(2));
        const auto path=Json::parse(row.path);
        item["origin"]="main_readback"; item["actionTool"]=path.at(0); item["readTool"]=path.at(1); item["actionRef"]=base+"/action/"+seq;
        item["receiptRef"]=base+"/receipt/"+seq; item["expected"]=preview(get.text(0)); item["actual"]=preview(get.text(1));
        item["met"]=get.text(0)==get.text(1); item["effect"]=get.text(3);
    }
    return item;
}
// 两条实际动作路径才唤起跨工具复盘，仅有来信不会代替独立观测。
std::optional<Json> MessageStore::next_case() const {
    std::lock_guard lock(state_->mutex); auto* db=state_->db;
    Statement next(db,"SELECT c.name,c.scope,c.cursor,c.head,max(m.id) FROM observation_case c JOIN case_member m ON m.name=c.name AND m.active=1 "
        "GROUP BY c.name HAVING max(m.id)>c.cursor AND count(DISTINCT CASE WHEN m.kind='tool_readback' THEN m.path END)>=2 "
        "ORDER BY min(CASE WHEN m.id>c.cursor THEN m.id END) LIMIT 1");
    if (!next.next()) return {};
    return batch_for(db,conversation(),next.text(0),Json::parse(next.text(1)),next.number(2),next.number(3),next.number(4));
}
// 模型只能引用实际收到的见证，分歧双方必须同时出现。
// 步骤：重核最新批次 → 校验字段与独立路径 → 保存本体解释并推进游标。
void MessageStore::commit_case(const Json& batch,const Json& proposal) {
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    const auto name=batch.at("case").get<std::string>();
    Statement current(db,"SELECT c.scope,c.cursor,c.head,max(m.id) FROM observation_case c JOIN case_member m ON m.name=c.name AND m.active=1 WHERE c.name=? GROUP BY c.name");
    current.bind(1,name);
    if (!current.next() || batch_for(db,conversation(),name,Json::parse(current.text(0)),current.number(1),current.number(2),current.number(3))!=batch)
        throw std::invalid_argument("comparison: stale evidence");
    auto head=current.number(2); const auto through=current.number(3);
    if (proposal!=Json{{"defer",true}}) {
        if (!proposal.is_object() || proposal.size()!=3 || !proposal.contains("text") || !proposal.at("text").is_string() ||
            proposal.at("text").get_ref<const std::string&>().find_first_not_of(" \r\n\t")==std::string::npos || proposal.at("text").dump().size()>322 ||
            !proposal.contains("judgement") || proposal.at("judgement")!=judgement(batch.at("state")) || !proposal.contains("cites") ||
            !proposal.at("cites").is_array() || proposal.at("cites").size()<2 || proposal.at("cites").size()>4)
            throw std::invalid_argument("comparison: invalid interpretation");
        std::set<std::string> paths,readings,refs;
        for(const auto& ref : proposal.at("cites")) {
            const auto& facts=batch.at("facts");
            if (!ref.is_string() || !refs.insert(ref.get<std::string>()).second ||
                std::none_of(facts.begin(),facts.end(),[&](const auto& f){return f.at("reference")==ref;}))
                throw std::invalid_argument("comparison: unseen citation");
            Statement get(db,"SELECT path,reading FROM case_member WHERE active=1 AND kind='tool_readback' AND reference=?");
            get.bind(1,ref.get_ref<const std::string&>()); if (get.next()) {paths.insert(get.text(0)); readings.insert(get.text(1));}
        }
        if (paths.size()<2 || ((batch.at("state")=="variable_result" || batch.at("state")=="variable_reading") && readings.size()<2))
            throw std::invalid_argument("comparison: missing independent paths");
        for(const auto& ref : batch.at("requiredConflict")) if (!refs.contains(ref.get<std::string>())) throw std::invalid_argument("comparison: missing conflicting observation");
        Statement add(db,"INSERT INTO case_opinion(name,parent,through_member,judgement,body,cites) VALUES(?,?,?,?,?,?)"); add.bind(1,name); add.bind(2,head);
        add.bind(3,through); add.bind(4,proposal.at("judgement").get_ref<const std::string&>()); add.bind(5,proposal.at("text").get_ref<const std::string&>());
        add.bind(6,proposal.at("cites").dump()); add.next(); head=sqlite3_last_insert_rowid(db);
    }
    Statement save(db,"UPDATE observation_case SET cursor=?,head=? WHERE name=?"); save.bind(1,through); save.bind(2,head); save.bind(3,name); save.next(); tx.commit();
}
// 查询一项已命名比较或精确版本。首屏优先交付分歧双方，继续翻页才展开其余原件。
// 步骤：解析引用与截止号 → 核对当前授权和版本 → 在编码预算内交付原件。
Json MessageStore::search_cases(const std::vector<std::string>& topics,unsigned count,std::size_t offset) const {
    Json out{{"items",Json::array()},{"limited",false}};
    if (topics.empty() || topics.size()>3 || !count) throw std::invalid_argument("comparison: expected one to three topics");
    for(const auto& t:topics) if(t.empty() || t.size()>160 || t.find_first_not_of(" \t\r\n")==t.npos)
        throw std::invalid_argument("comparison: invalid topic");
    std::lock_guard lock(state_->mutex); auto* db=state_->db;
    auto name=topics.front(); const auto prefix=conversation()+"/comparison/", history=conversation()+"/comparison-opinion/";
    std::int64_t requested=0,cutoff=INT64_MAX;
    const auto reference=[](const std::string& t){return t.find("/comparison/")!=t.npos || t.find("/comparison-opinion/")!=t.npos;};
    if (topics.size()>1 && std::ranges::any_of(topics,reference)) return out;
    if (name.starts_with(history)) {
        const auto s=std::string_view(name).substr(history.size()); const auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),requested);
        if (ec!=std::errc{} || end!=s.data()+s.size() || requested<=0 || s.front()=='0') return out;
        Statement get(db,"SELECT name,through_member FROM case_opinion WHERE id=? AND valid=1"); get.bind(1,requested); if(!get.next()) return out;
        name=get.text(0); cutoff=get.number(1);
    } else if (name.starts_with(prefix)) name.erase(0,prefix.size());
    else {
        if(reference(name)) return out;
        bool ambiguous=false; name=resolve_case_terms(db,topics,ambiguous);
        if (name.empty()) {if (ambiguous) out["ambiguous"]=true; return out;}
    }
    Statement get(db,"SELECT scope,head,(SELECT max(id) FROM case_member m WHERE m.name=c.name AND active=1) FROM observation_case c WHERE name=?");
    get.bind(1,name); if(!get.next()) return out;
    const auto scope=Json::parse(get.text(0)); const auto evidence=classify(db,name,scope,cutoff);
    if (offset>=evidence.total) return out;
    Json item{{"reference",prefix+name},{"scope",scope},{"state",evidence.state},{"nativeCount",evidence.natives},
        {"reportCount",evidence.total-evidence.natives},{"pathCount",evidence.paths},{"facts",Json::array()}};
    if (!evidence.conflict.empty()) item["conflictRefs"]=evidence.conflict;
    const auto head=get.number(1),id=requested ? requested : head;
    if (auto authored=opinion(db,conversation(),id); !authored.is_null()) {
        Statement last(db,"SELECT through_member FROM case_opinion WHERE id=?"); last.bind(1,id); last.next();
        authored["standing"]=id!=head || (requested && cutoff<get.number(2)) ? "historical" : authored.at("judgement")!=judgement(evidence.state) ? "challenged" :
            !requested && last.number(0)<evidence.latest ? "awaiting_review" : "current";
        item["opinion"]=std::move(authored);
    }
    const auto take=std::max<std::size_t>(!offset ? evidence.conflict.size() : 0,std::min(count,3u));
    const auto selected=case_review::page(db,name,cutoff,evidence,offset,take);
    std::size_t end=offset; std::vector<Json> full;
    const auto required=!offset ? std::max<std::size_t>(1,evidence.conflict.size()) : 1;
    // 先保留必需的引用，分歧一方不会因另一方的长读数挤掉。剩余额度逐份补回完整字段。
    for(;end-offset<selected.size() && end-offset<required;++end) {
        auto f=fact(db,selected[end-offset]);
        item["facts"].push_back({{"reference",f.at("reference")},{"origin",f.at("origin")},{"checkpoint",f.at("checkpoint")},{"partial",true}});
        full.push_back(std::move(f));
    }
    for(std::size_t i=0;i<full.size();++i) {
        auto& slot=item["facts"][i]; auto compact=slot; slot=full[i];
        if(item.dump().size()+48>2048) slot=std::move(compact);
    }
    for(;end-offset<selected.size();++end) {
        item["facts"].push_back(fact(db,selected[end-offset]));
        if(item.dump().size()+48>2048) {item["facts"].erase(item["facts"].end()-1); break;}
    }
    if (end<evidence.total) {item["nextOffset"]=end; out["limited"]=true;}
    out["items"].push_back(std::move(item));
    return out;
}
}
