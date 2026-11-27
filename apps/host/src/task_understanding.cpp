#include "message_store_db.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <set>

namespace dfy::app {
using namespace message_db;
using Json = nlohmann::json;

// 只索引本体实际派发、取得工作回执并由同一读口前后核验的原任务。
void message_db::index_task_observation(sqlite3* db, std::int64_t id, const Json& task) {
    if (!task.contains("feedback") || !task.contains("expected")) return;
    const auto& works = task.at("works");
    if (works.size()!=3 || !works[0].value("dispatched",false) || !works[0].value("succeeded",false) ||
        !works[1].value("dispatched",false) || !works[1].value("succeeded",false) || works[1].at("toolId")!=task.at("tool") ||
        !works[2].value("dispatched",false) || !works[2].value("succeeded",false) ||
        works[0].at("toolId")!=works[2].at("toolId")) return;
    const auto family = Json::array({task.at("tool"),works[0].at("toolId")}).dump();
    Statement add(db,"INSERT INTO task_observation(task,family,before_value,expected,actual,action,baseline,readback,effect) VALUES(?,?,?,?,?,?,?,?,?)");
    add.bind(1,id); add.bind(2,family); add.bind(3,works[0].at("output").get_ref<const std::string&>());
    add.bind(4,task.at("expected").get_ref<const std::string&>()); add.bind(5,works[2].at("output").get_ref<const std::string&>());
    add.bind(6,works[1].at("candidate").get<std::int64_t>()); add.bind(7,works[0].at("candidate").get<std::int64_t>());
    add.bind(8,works[2].at("candidate").get<std::int64_t>());
    add.bind(9,works[1].at("effect").get_ref<const std::string&>()); add.next();
    Statement group(db,"INSERT OR IGNORE INTO reflection_progress(family) VALUES(?)");
    group.bind(1,family); group.next();
}

namespace {
// 对只作展示的长文本保留明确的省略标志，核验始终读完整值。
std::string brief(std::string value) {
    if (value.size()<=96 && Json(value).dump().size()<=98) return value;
    auto end = std::min<std::size_t>(value.size(),93);
    for (;;) {
        while (end<value.size() && (static_cast<unsigned char>(value[end])&0xc0)==0x80) --end;
        auto part=value.substr(0,end);
        if (Json(part).dump().size()<=95) return part+"…";
        end/=2;
    }
}
// 把一项可检验判断应用在原始采样上；未落入条件的任务单独归类。
std::string role(const Json& claim, const Statement& row) {
    if (claim.is_null()) return "observation";
    const auto before = row.text(1), expected = row.text(2), actual = row.text(3);
    if ((claim.at("condition")=="before" && claim.at("when")!=before) ||
        (claim.at("condition")=="expected" && claim.at("when")!=expected)) return "outside";
    const auto& prediction = claim.at("predict");
    const bool hit = prediction=="target" ? actual==expected : prediction=="changed" ? actual!=before :
        prediction=="unchanged" ? actual==before : claim.at("value")==actual;
    return hit ? "support" : "counterexample";
}
// 读取轻量的原任务索引，行号和候选号仍与不可变工作回执共用。
constexpr char observations[] = "SELECT task,before_value,expected,actual,action,baseline,readback,effect,substr(goal,1,97) "
    "FROM task_observation JOIN task_memory ON id=task WHERE family=? AND task<=? ORDER BY task";
// 以档案身份签发配套出处，可沿它们展开完整参数、原回执及读回值。
Json fact(const std::string& profile, const Statement& row) {
    const auto base = profile+"/task/"+std::to_string(row.number(0)), action = std::to_string(row.number(4));
    return {{"taskRef",base},{"actionRef",base+"/action/"+action},{"receiptRef",base+"/receipt/"+action},
        {"baselineRef",base+"/receipt/"+std::to_string(row.number(5))},{"readbackRef",base+"/receipt/"+std::to_string(row.number(6))},
        {"before",brief(row.text(1))},{"expected",brief(row.text(2))},{"actual",brief(row.text(3))},
        {"verification",row.text(2)==row.text(3) ? "confirmed" : "contradicted"},{"effect",row.text(7)},{"goal",brief(row.text(8))}};
}
// 取仍获准的原版本；空值表示还没有可继承的解释。
Json claim_at(sqlite3* db, std::int64_t id) {
    Statement get(db,"SELECT claim FROM task_understanding WHERE id=? AND valid=1");
    get.bind(1,id);
    return get.next() ? Json::parse(get.text(0)) : Json(nullptr);
}
// 为这一截止号选取新经历、先前独立依据及反例，模型一次最多读四份。
Json comparison(sqlite3* db, const std::string& profile, const std::string& family, std::int64_t cursor,
    std::int64_t head, std::int64_t through) {
    const auto claim = claim_at(db,head);
    Json basis=Json::array(), contrary, latest;
    // 从最近往回扫描，只装配被选中的原件；旧反例优先占用一个独立位置。
    Statement rows(db,"SELECT task,before_value,expected,actual,action,baseline,readback,effect,substr(goal,1,97) "
        "FROM task_observation JOIN task_memory ON id=task WHERE family=? AND task<=? ORDER BY task DESC");
    rows.bind(1,family); rows.bind(2,through);
    while (rows.next()) {
        const auto standing = role(claim,rows);
        if (rows.number(0)!=through && (standing=="outside" || (standing=="counterexample" ? !contrary.is_null() : basis.size()==2))) continue;
        auto item = fact(profile,rows);
        item["priorRole"] = standing;
        if (rows.number(0)==through) latest = std::move(item);
        else if (standing=="counterexample") contrary = std::move(item);
        else basis.insert(basis.begin(),std::move(item));
        if (basis.size()==2 && (claim.is_null() || !contrary.is_null())) break;
    }
    if (!contrary.is_null()) basis.push_back(std::move(contrary));
    if (latest.is_null()) throw std::invalid_argument("reflection: missing original");
    basis.push_back(std::move(latest));
    return {{"family",family},{"cursor",cursor},{"head",head},{"through",through},{"prior",claim},{"facts",std::move(basis)}};
}
// 拒绝没有适用条件、可检验预测或可展示解释正文的提议。
bool valid_claim(const Json& claim) {
    if (!claim.is_object() || claim.size()!=5) return false;
    for (const auto key : {"text","condition","when","predict","value"})
        if (!claim.contains(key) || !claim.at(key).is_string()) return false;
    const auto& text = claim.at("text").get_ref<const std::string&>();
    const auto& c = claim.at("condition"), &p = claim.at("predict");
    return text.size()<=320 && claim.dump().size()<=620 && text.find_first_not_of(" \t\r\n")!=text.npos &&
        (c=="any" || c=="before" || c=="expected") && (c!="any" || claim.at("when")=="") &&
        (p=="target" || p=="changed" || p=="unchanged" || p=="value") && (p=="value" || claim.at("value")=="") &&
        claim.at("when").get_ref<const std::string&>().size()<=128 && claim.at("value").get_ref<const std::string&>().size()<=128;
}
// 解释和任务编号共用本档案命名空间，完整解析防止跨档案及截断编号命中。
std::int64_t ref_id(std::string_view value) {
    std::int64_t id=0;
    const auto [end,err] = std::from_chars(value.data(),value.data()+value.size(),id);
    if (err!=std::errc{} || end!=value.data()+value.size() || id<=0 || value.front()=='0')
        throw std::invalid_argument("reflection: invalid reference");
    return id;
}

// 对当前解释按不同主题命中数、精确目标或工具号排序，同分时保留问题的歧义。
// 每个主题在每组至多计一次，重复任务、历史版本和未授权的原件不增加权重。
std::int64_t match_understanding(sqlite3* db,const std::vector<std::string>& topics,bool& ambiguous) {
    Statement find(db,
        "WITH words(term) AS (VALUES (?1),(?2),(?3)), terms AS (SELECT DISTINCT term FROM words WHERE term<>'') ,"
        "hits(head,term,exact) AS ("
        "SELECT p.head,q.term,max(t.goal=q.term OR t.tool=q.term) FROM terms q JOIN task_observation o "
        "JOIN task_memory t ON t.id=o.task JOIN reflection_progress p ON p.family=o.family "
        "WHERE p.head>0 AND t.active=1 AND (instr(t.goal,q.term)>0 OR instr(t.tool,q.term)>0) GROUP BY p.head,q.term "
        "UNION ALL SELECT u.id,q.term,0 FROM terms q JOIN reflection_progress p JOIN task_understanding u ON u.id=p.head "
        "WHERE u.valid=1 AND instr(u.recall_text,q.term)>0) "
        "SELECT head,count(DISTINCT term) AS coverage,max(exact) AS identity_hit FROM hits GROUP BY head "
        "ORDER BY coverage DESC,identity_hit DESC,head LIMIT 2");
    for (int i=0;i<3;++i) find.bind(i+1,i<static_cast<int>(topics.size()) ? topics[i] : std::string{});
    if (!find.next()) return 0;
    const auto id=find.number(0), coverage=find.number(1), exact=find.number(2);
    ambiguous=find.next() && coverage==find.number(1) && exact==find.number(2);
    return ambiguous ? 0 : id;
}
}

// 每组首次取两次独立经历，以后逐次处理，离线积压也不能跳过中间的反例。
std::optional<Json> MessageStore::next_comparison() const {
    std::lock_guard lock(state_->mutex);
    Statement group(state_->db,"SELECT family,cursor,head,(SELECT min(task) FROM task_observation o WHERE o.family=p.family AND task>p.cursor) AS upcoming "
        "FROM reflection_progress p WHERE upcoming IS NOT NULL AND EXISTS(SELECT 1 FROM task_observation o WHERE o.family=p.family LIMIT 1 OFFSET 1) "
        "ORDER BY upcoming LIMIT 1");
    if (!group.next()) return {};
    const auto family = group.text(0);
    auto through = group.number(3);
    if (!group.number(1)) {
        Statement second(state_->db,"SELECT task FROM task_observation WHERE family=? ORDER BY task LIMIT 1 OFFSET 1");
        second.bind(1,family); second.next(); through = second.number(0);
    }
    return comparison(state_->db,conversation(),family,group.number(1),group.number(2),through);
}

// 在同一事务复核获准原件与旧版本；自由文本始终只作为本体理解保存。
// 步骤：复核来源 → 检验每条预测与引用 → 判定支持或修订 → 追加版本并移动进度。
void MessageStore::commit_understanding(const Json& batch, const Json& proposal) {
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction tx(db);
    const auto family = batch.at("family").get<std::string>();
    const auto head = batch.at("head").get<std::int64_t>(), through = batch.at("through").get<std::int64_t>();
    Statement group(db,"SELECT cursor,head FROM reflection_progress WHERE family=?");
    group.bind(1,family);
    if (!group.next() || group.number(0)!=batch.at("cursor").get<std::int64_t>() || group.number(1)!=head ||
        comparison(db,conversation(),family,group.number(0),head,through)!=batch) throw std::invalid_argument("reflection: stale evidence");
    auto next = head;
    if (proposal!=Json{{"defer",true}}) {
        if (!proposal.is_object() || proposal.size()!=2 || !proposal.contains("claim") || !valid_claim(proposal.at("claim")) ||
            !proposal.contains("cites") || !proposal.at("cites").is_array()) throw std::invalid_argument("reflection: invalid proposal");
        const auto& claim = proposal.at("claim"), &prior = batch.at("prior");
        std::set<std::string> cited;
        for (const auto& ref : proposal.at("cites")) {
            if (!ref.is_string() || !cited.insert(ref.get<std::string>()).second ||
                std::ranges::none_of(batch.at("facts"),[&](const auto& item){return item.at("taskRef")==ref;}))
                throw std::invalid_argument("reflection: unseen or repeated evidence");
        }
        if (!cited.contains(batch.at("facts").back().at("taskRef").get<std::string>())) throw std::invalid_argument("reflection: missing new event");
        unsigned supports = 0;
        bool challenge = false, latest_support = false;
        Statement rows(db,observations);
        rows.bind(1,family); rows.bind(2,through);
        while (rows.next()) {
            const auto standing = role(claim,rows);
            if (standing=="counterexample") throw std::invalid_argument("reflection: unsupported prediction");
            if (cited.contains(conversation()+"/task/"+std::to_string(rows.number(0)))) {
                if (standing=="support") ++supports;
                if (role(prior,rows)=="counterexample") challenge = true;
                if (rows.number(0)==through) latest_support = standing=="support";
            }
        }
        if (supports<2) throw std::invalid_argument("reflection: insufficient independent support");
        std::string relation = "formed";
        if (!prior.is_null()) {
            const bool prediction = claim.at("predict")==prior.at("predict") && claim.at("value")==prior.at("value");
            const bool scope = claim.at("condition")==prior.at("condition") && claim.at("when")==prior.at("when");
            if (prediction && scope && latest_support) relation = "reinforced";
            else if (challenge && prediction && prior.at("condition")=="any" && !scope) relation = "narrowed";
            else if (challenge && (!prediction || !scope)) relation = "revised";
            else throw std::invalid_argument("reflection: missing counterexample");
        }
        Statement add(db,"INSERT INTO task_understanding(family,parent,through_task,relation,claim,cites,contrast,recall_text) "
            "VALUES(?1,?2,?3,?4,?5,?6,CASE WHEN ?4 IN ('narrowed','revised') THEN ?2 ELSE "
            "coalesce((SELECT contrast FROM task_understanding WHERE id=?2),0) END,?7)");
        add.bind(1,family); add.bind(2,head); add.bind(3,through); add.bind(4,relation); add.bind(5,claim.dump());
        add.bind(6,proposal.at("cites").dump()); add.bind(7,claim.at("text").get_ref<const std::string&>()); add.next();
        next = sqlite3_last_insert_rowid(db);
    }
    Statement save(db,"UPDATE reflection_progress SET cursor=?,head=? WHERE family=?");
    save.bind(1,through); save.bind(2,next); save.bind(3,family); save.next();
    complete_idle_queue(db,IdleQueue::tool_family); tx.commit();
}

// 独立装配本体解释和可展开的原始观察；反例先展示，旧版本仍按当时的依据解释。
Json MessageStore::search_understandings(const std::vector<std::string>& topics, unsigned count, std::size_t offset) const {
    if (topics.empty() || topics.size()>3 || !count || std::ranges::any_of(topics,[](const auto& term){
        return term.size()>160 || term.find_first_not_of(" \t\r\n")==term.npos;
    })) throw std::invalid_argument("reflection: invalid topics");
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    const auto& term = topics.front();
    const auto prefix = conversation()+"/understanding/", task_prefix = conversation()+"/task/";
    Json result{{"items",Json::array()},{"limited",false}};
    std::int64_t id=0;
    if (std::ranges::any_of(topics,[](const auto& t){return t.find("/task/")!=t.npos || t.find("/understanding/")!=t.npos;})) {
        if (topics.size()!=1) return result;
        if (term.starts_with(prefix)) id=ref_id(std::string_view(term).substr(prefix.size()));
        else if (term.starts_with(task_prefix)) {
            Statement match(db,"SELECT p.head FROM reflection_progress p JOIN task_observation o ON o.family=p.family WHERE o.task=?");
            match.bind(1,ref_id(std::string_view(term).substr(task_prefix.size())));
            if (match.next()) id=match.number(0);
        }
    } else {
        bool ambiguous=false;
        id=match_understanding(db,topics,ambiguous);
        if (ambiguous) { result["ambiguous"]=true; return result; }
    }
    Statement version(db,"SELECT u.family,u.parent,u.through_task,u.relation,u.claim,u.cites,p.head,u.contrast,"
        "(SELECT v.through_task FROM task_understanding v WHERE v.family=u.family AND v.parent=u.contrast AND v.valid=1 AND u.contrast>0 "
        "AND v.id<=u.id ORDER BY v.id DESC LIMIT 1) FROM task_understanding u "
        "JOIN reflection_progress p ON u.family=p.family WHERE u.id=? AND u.valid=1");
    version.bind(1,id);
    if (!version.next()) return result;
    const auto family = version.text(0);
    const auto prior = claim_at(db,version.number(7)), claim = Json::parse(version.text(4));
    const auto cites = Json::parse(version.text(5)).get<std::vector<std::string>>();
    const bool current = version.number(6)==id;
    const auto through = version.number(2);
    Json item{{"reference",prefix+std::to_string(id)},{"author","main"},{"kind","interpretation"},{"relation",version.text(3)},
        {"standing",current ? "current" : "superseded"},{"claim",claim},{"facts",Json::array()}};
    if (version.number(1)) item["previousReference"] = prefix+std::to_string(version.number(1));
    if (version.number(7)) item["revisedFrom"] = prefix+std::to_string(version.number(7));
    enum Priority : std::size_t { challenge, past_challenge, cited_support, other_support, omitted };
    std::array<std::size_t,omitted> totals{};
    std::size_t support=0;
    std::vector<std::pair<std::size_t,Json>> page;
    // 两趟扫描先计数再取页；不论档案大小，仅装配本页的原件。
    for (bool collect : {false,true}) {
        std::array<std::size_t,omitted> positions{};
        for (std::size_t n=1;n<positions.size();++n) positions[n]=positions[n-1]+totals[n-1];
        Statement rows(db,observations);
        rows.bind(1,family); rows.bind(2,current ? INT64_MAX : through);
        while (rows.next()) {
            const auto standing = role(claim,rows);
            if (rows.number(0)>through && standing!="counterexample") continue;
            const bool against_prior = rows.number(0)<=version.number(8) && role(prior,rows)=="counterexample";
            if (standing=="outside" && !against_prior) continue;
            const auto ref=task_prefix+std::to_string(rows.number(0));
            const bool cited=std::ranges::find(cites,ref)!=cites.end();
            const auto priority=standing=="counterexample" ? challenge : against_prior ? past_challenge : cited ? cited_support : other_support;
            if (!collect) {
                ++totals[priority];
                if (standing=="support") ++support;
                continue;
            }
            const auto position=positions[priority]++;
            if (position<offset || position-offset>=count) continue;
            auto evidence = fact(conversation(),rows);
            evidence["role"] = standing;
            evidence["challengedPrevious"] = against_prior;
            evidence["cited"] = cited;
            page.emplace_back(position,std::move(evidence));
        }
    }
    const auto seen=totals[challenge]+totals[past_challenge]+totals[cited_support]+totals[other_support];
    if (offset>seen) throw std::invalid_argument("reflection: invalid evidence offset");
    item["supportCount"] = support; item["counterexampleCount"] = totals[challenge];
    if (current && totals[challenge]) item["standing"] = "challenged";
    std::ranges::sort(page,{},&std::pair<std::size_t,Json>::first);
    for (auto& [position,evidence] : page) {
        item["facts"].push_back(std::move(evidence));
        // 为下一页位置及数组边界预留空间，超预算的原件从下一页开始交付。
        if (item.dump().size()>2000) { item["facts"].erase(item["facts"].end()-1); break; }
    }
    const auto next=offset+item.at("facts").size();
    if (next<seen) { result["limited"]=true; item["nextOffset"]=next; }
    result["items"].push_back(std::move(item)); return result;
}
}
