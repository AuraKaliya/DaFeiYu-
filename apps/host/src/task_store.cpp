#include "message_store_db.hpp"
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <charconv>
#include <map>

namespace dfy::app {
using namespace message_db;
using Json = nlohmann::json;
namespace {
// 解析本档案内的正整数原件号，拒绝部分匹配和别名。
std::int64_t positive_id(std::string_view value) {
    std::int64_t id = 0;
    const auto parsed = std::from_chars(value.data(),value.data()+value.size(),id);
    if (parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || id<=0 || value.front()=='0')
        throw std::invalid_argument("task memory: invalid identity");
    return id;
}
// 在 UTF-8 字符边界切分原文，分段预算按实际 JSON 转义后的长度计量。
std::string excerpt(const std::string& text, std::size_t offset, std::size_t budget) {
    if (offset>text.size() || (offset<text.size() && (static_cast<unsigned char>(text[offset])&0xc0)==0x80))
        throw std::invalid_argument("task memory: invalid byte offset");
    auto bytes = std::min(text.size()-offset,budget-2);
    for (;;) {
        while (bytes && offset+bytes<text.size() && (static_cast<unsigned char>(text[offset+bytes])&0xc0)==0x80) --bytes;
        auto part = text.substr(offset,bytes);
        if (Json(part).dump().size()<=budget) return part;
        bytes /= 2;
    }
}
// 展示短摘要时显式标明省略，逐字原件另按身份读取。
std::string summary(const std::string& text, std::size_t budget) {
    auto part = excerpt(text,0,budget);
    if (part.size()<text.size()) part += "…";
    return part;
}
// 归纳当前提议是否真正进入工具实现；缺少终态原件时保留未核实状态。
std::string execution(const Json& work) {
    if (!work.contains("dispatched")) return work.at("accepted")==true ? "dispatch_unknown" : "proposed";
    return work.at("dispatched")==true ? "dispatched" : "not_dispatched";
}
// 从原运行中的同一候选签发相互可追溯的行动号和回执号。
std::string work_ref(const std::string& base, std::string_view kind, const Json& work) {
    return base+"/"+std::string(kind)+"/"+std::to_string(work.at("candidate").get<std::uint64_t>());
}
// 任务概览注明所针对的工具，并从第一项提议提供逐项展开其余工作的入口。
Json overview(const Json& task, const std::string& base, const std::string& reason, const std::string& review) {
    const auto& works = task.at("works");
    Json item{{"reference",base},{"kind","task"},{"origin",task.at("origin")},{"tool",task.at("tool")},{"provider",task.at("provider")},
        {"utcMs",task.at("utcMs")},{"goal",summary(task.at("goal"),180)},{"goalRef",base+"/goal"},{"runResult",task.at("runResult")},
        {"count",works.size()},{"execution","not_proposed"},{"verification",task.at("verification")},
        {"reviewState",reason.empty() ? "facts_only" : review.empty() ? "queued" : "reviewed"}};
    for (const auto key : {"plan","expected"}) if (task.contains(key)) item[key] = summary(task.at(key),160);
    if (!works.empty()) item["firstAction"] = work_ref(base,"action",works.front());
    if (task.contains("primary")) {
        const auto& primary = works.at(task.at("primary").get<std::size_t>());
        item["actionRef"] = work_ref(base,"action",primary);
        item["execution"] = execution(primary);
        if (primary.contains("result")) {
            item["effect"] = primary.at("effect");
            item["receiptRef"] = work_ref(base,"receipt",primary);
        }
    }
    if (task.contains("feedback")) {
        item["feedback"] = task.at("feedback");
        item["baselineRef"] = work_ref(base,"receipt",works.front());
        item["readbackRef"] = work_ref(base,"receipt",works.back());
    }
    if (!reason.empty()) item["reviewReason"] = reason;
    if (!review.empty()) item["review"] = review;
    return item;
}
// 按任务原号展开目标、计划、工具原参数或原回执；只有真实终态产生回执号。
std::optional<Json> original(const Json& task, const std::string& base, const std::string& path, std::size_t offset) {
    Json item{{"reference",base+"/"+path},{"taskRef",base},{"origin",task.at("origin")},{"provider",task.at("provider")}};
    const std::string* payload = nullptr;
    if (path=="goal" || path=="plan" || path=="expected") {
        if (!task.contains(path)) return {};
        item["kind"] = path;
        payload = &task.at(path).get_ref<const std::string&>();
    } else {
        const auto split = path.find('/');
        const auto kind = path.substr(0,split);
        if (split==path.npos || (kind!="action" && kind!="receipt")) return {};
        const auto candidate = positive_id(std::string_view(path).substr(split+1));
        const auto& works = task.at("works");
        const auto it = std::ranges::find_if(works,[&](const Json& work){return work.at("candidate")==candidate;});
        if (it==works.end() || (kind=="receipt" && !it->contains("result"))) return {};
        item["kind"] = kind;
        item["tool"] = it->value("toolId",it->at("name").get<std::string>());
        item["execution"] = execution(*it);
        item["actionRef"] = work_ref(base,"action",*it);
        if (it->contains("result")) {
            item["receiptRef"] = work_ref(base,"receipt",*it);
            item["effect"] = it->at("effect");
            item["result"] = it->at("result");
        }
        auto& identity = item["work"] = Json::object();
        for (const auto key : {"candidate","invocation","step","attempt","callId","name","confirmation","accepted","dispatched","succeeded","error","fault"})
            if (it->contains(key)) identity[key] = it->at(key);
        if (std::next(it)!=works.end()) item["nextAction"] = work_ref(base,"action",*std::next(it));
        payload = &it->at(kind=="action" ? "arguments" : "output").get_ref<const std::string&>();
    }
    auto part = excerpt(*payload,offset,640);
    if (offset+part.size()<payload->size()) item["nextOffset"] = offset+part.size();
    item["payload"] = std::move(part);
    return item;
}
}

// 在同一事务中保存不可变运行证据和待办原因，相同转变只提请首次复盘。
void MessageStore::save_task(const Json& task) {
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction tx(db);
    const auto origin = task.at("origin").get<std::string>(), body = task.dump();
    Statement existing(db,"SELECT body FROM task_memory WHERE origin=?");
    existing.bind(1,origin);
    if (existing.next()) {
        if (existing.text(0)!=body) throw std::logic_error("task memory: run identity conflict");
        tx.commit(); return;
    }
    auto reason = task.value("reason",std::string{});
    const auto signature = task.value("signature",std::string{});
    if (!signature.empty()) {
        Statement known(db,"SELECT 1 FROM task_memory WHERE active=1 AND signature=? LIMIT 1");
        known.bind(1,signature);
        if (known.next()) reason.clear();
    }
    Statement add(db,"INSERT INTO task_memory(origin,goal,tool,body,reason,signature) VALUES(?,?,?,?,?,?)");
    add.bind(1,origin); add.bind(2,task.at("goal").get_ref<const std::string&>()); add.bind(3,task.at("tool").get_ref<const std::string&>());
    add.bind(4,body); add.bind(5,reason); add.bind(6,signature); add.next();
    const auto id=sqlite3_last_insert_rowid(db); index_task_observation(db,id,task);
    if (task.contains("comparison")) {
        Statement read(db,"SELECT readback FROM task_observation WHERE task=?"); read.bind(1,id);
        if (read.next()) {
            const auto& tag=task.at("comparison");
            link_case_member(db,conversation(),tag.at("name"),tag.at("checkpoint"),conversation()+"/task/"+std::to_string(id)+
                "/receipt/"+std::to_string(read.number(0)),"at_start");
        }
    }
    sql(db,"UPDATE conversation SET memory_revision=1 WHERE memory_revision=0");
    tx.commit();
}
// 空闲时取一份待办，按当时的实际工作和核验结果写回结论，不发起模型或工具调用。
bool MessageStore::review_idle_task() {
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction tx(db);
    Statement next(db,"SELECT id,reason,body FROM task_memory WHERE active=1 AND reason<>'' AND review='' "
        "ORDER BY CASE reason WHEN 'unexpected_result' THEN 0 WHEN 'unverified_effect' THEN 1 ELSE 2 END,id LIMIT 1");
    if (!next.next()) { tx.commit(); return false; }
    const auto task = Json::parse(next.text(2));
    const auto reason = next.text(1);
    std::string note;
    if (reason=="unverified_effect") {
        const auto& action = task.at("works").at(task.at("primary").get<std::size_t>());
        note = action.at("effect")=="applied" ? "工具回执记为已生效。下次接续前需读回目标状态。" :
            "工具确已派发，回执中的实效未知。重试前需查原工作。";
    } else {
        const auto& works = task.at("works");
        const auto after = summary(works.back().at("output"),80);
        note = reason=="unexpected_result" ? "我原要得到「"+summary(task.at("expected"),80)+"」，动作后读回「"+after+"」，目标未达成。" :
            "这次动作前读到「"+summary(works.front().at("output"),80)+"」，动作后读到「"+after+"」，与目标一致。";
    }
    Statement update(db,"UPDATE task_memory SET review=? WHERE id=?");
    update.bind(1,note); update.bind(2,next.number(0)); update.next(); tx.commit();
    return true;
}
// 只为任务卡投影当前可读的身份、目标及时间，原回执留在原档案中。
Json MessageStore::recent_tasks() const {
    std::lock_guard lock(state_->mutex);
    Statement rows(state_->db,"SELECT id,goal,body FROM task_memory WHERE active=1 ORDER BY id DESC LIMIT 12");
    Json cards = Json::array();
    while (rows.next()) {
        const auto task = Json::parse(rows.text(2));
        cards.push_back({{"reference",conversation()+"/task/"+std::to_string(rows.number(0))},
            {"goal",summary(rows.text(1),180)},{"utcMs",task.at("utcMs")}});
    }
    return cards;
}

// 合并主题与原件号命中，按精确程度和新近顺序，在单轮预算内装入证据。
Json MessageStore::search_tasks(const std::vector<std::string>& topics, unsigned count, std::size_t offset) const {
    if (topics.empty() || topics.size()>3 || !count) throw std::invalid_argument("task memory: invalid query");
    const auto prefix = conversation()+"/task/";
    std::lock_guard lock(state_->mutex);
    std::map<std::pair<std::int64_t,std::string>,int> ranks;
    for (const auto& term : topics) {
        if (term.starts_with(prefix)) {
            const std::string_view rest = std::string_view(term).substr(prefix.size());
            const auto slash = rest.find('/');
            ranks[{positive_id(rest.substr(0,slash)),slash==rest.npos ? "" : std::string(rest.substr(slash+1))}] = 0;
        } else {
            if (term.empty() || term.size()>128 || term.find_first_not_of(" \t\r\n")==term.npos || offset)
                throw std::invalid_argument("task memory: invalid topic");
            if (term.find("/task/")!=term.npos) continue;
            Statement matches(state_->db,"SELECT id,CASE WHEN goal=?1 OR tool=?1 THEN 1 ELSE 2 END AS rank FROM task_memory "
                "WHERE active=1 AND (instr(goal,?1)>0 OR instr(tool,?1)>0) ORDER BY rank,id DESC LIMIT 4");
            matches.bind(1,term);
            while (matches.next()) {
                auto [it,added] = ranks.emplace(std::pair{matches.number(0),std::string{}},static_cast<int>(matches.number(1)));
                if (!added) it->second = std::min(it->second,static_cast<int>(matches.number(1)));
            }
        }
    }
    std::vector<std::tuple<int,std::int64_t,std::string>> order;
    for (const auto& [key,rank] : ranks) order.emplace_back(rank,-key.first,key.second);
    std::ranges::sort(order);
    Json items = Json::array();
    bool limited = false;
    for (const auto& [rank,negative,path] : order) {
        Statement row(state_->db,"SELECT body,reason,review FROM task_memory WHERE id=? AND active=1");
        row.bind(1,-negative);
        if (!row.next()) continue;
        const auto base = prefix+std::to_string(-negative);
        const auto task = Json::parse(row.text(0));
        if (offset && path.empty()) throw std::invalid_argument("task memory: original required");
        const auto item = path.empty() ? std::optional(overview(task,base,row.text(1),row.text(2))) : original(task,base,path,offset);
        if (!item) continue;
        if (items.size()>=count) { limited = true; break; }
        items.push_back(*item);
        if (items.dump().size()>2048) { items.erase(items.end()-1); limited = true; }
        else if (item->contains("nextOffset")) limited = true;
    }
    return {{"items",std::move(items)},{"limited",limited}};
}
}
