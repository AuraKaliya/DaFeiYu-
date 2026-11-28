#include "message_store_db.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/plugin_agents.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <charconv>
#include <set>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
// 知识键与版本标签供命令重试使用，只接收规范标识。
void key(std::string_view value) {
    if (value.empty() || value.size()>64 || value.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_")!=value.npos)
        throw std::invalid_argument("knowledge: invalid article or edition");
}
// 验证要交给读者的正文，按编码长度预留来源与权限字段空间。
void prose(const std::string& value,std::size_t bytes) {
    if (!runtime::valid_utf8(value) || value.find_first_not_of(" \t\r\n")==value.npos || Json(value).dump().size()>bytes)
        throw std::invalid_argument("knowledge: invalid text or scope");
}
// 完整解析宿主签发的正整数，防止部分匹配和非规范原件号。
std::int64_t number(std::string_view s) {
    std::int64_t id=0;
    const auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),id);
    if(ec!=std::errc{} || end!=s.data()+s.size() || id<=0 || s.front()=='0') throw std::invalid_argument("knowledge: invalid source");
    return id;
}
// 核对依据原件类型和此刻的读取权，保留原作者类别及撤权依赖。
std::optional<MemoryBasis> basis(sqlite3* db,const std::string& profile,const std::string& ref) {
    if (!ref.starts_with(profile+"/")) return {};
    auto tail=std::string_view(ref).substr(profile.size()+1);
    if (tail.starts_with("task/")) {
        tail.remove_prefix(5); const auto split=tail.find("/receipt/");
        if (split==tail.npos) return {};
        const auto task=number(tail.substr(0,split)), receipt=number(tail.substr(split+9));
        Statement get(db,"SELECT 1 FROM task_observation o JOIN task_memory t ON t.id=o.task WHERE o.task=? AND o.readback=? AND t.active=1");
        get.bind(1,task); get.bind(2,receipt);
        if (get.next()) return MemoryBasis{"tool_readback",profile+"/task/"+std::to_string(task),task};
    } else if (tail.starts_with("letter/")) {
        Statement get(db,"SELECT source FROM readable_letter WHERE id=? AND phase='end'");
        const auto id=number(tail.substr(7)); get.bind(1,id);
        if (get.next()) return MemoryBasis{"sender_report",get.number(0) ? profile+"/"+std::to_string(get.number(0)) : ref,id};
    } else if (tail.starts_with("document/")) {
        Statement get(db,"SELECT 1 FROM knowledge_document WHERE reference=? AND active=1");
        get.bind(1,ref);
        if (get.next()) return MemoryBasis{"external_document",ref,0};
    } else if (tail.find('/')==tail.npos) {
        Statement get(db,"SELECT 1 FROM personal_memory WHERE active=1 AND message=?"); get.bind(1,number(tail));
        if (get.next()) return MemoryBasis{"owner_statement",ref,number(tail)};
    }
    return {};
}
// 回到仍获准的原始来源；任一必要依据失效时整版停止交付。
bool references(sqlite3* db,const std::string& profile,std::int64_t edition,Json& refs,std::vector<std::string>* deps=nullptr) {
    Statement get(db,"SELECT reference,kind,dependency FROM knowledge_basis WHERE edition=? ORDER BY position");
    get.bind(1,edition);
    while (get.next()) {
        const auto ref=get.text(0); const auto source=basis(db,profile,ref);
        if (!source || source->kind!=get.text(1) || source->dependency!=get.text(2)) return false;
        refs.push_back({{"kind",source->kind},{"reference",ref}});
        if (deps) deps->push_back(source->dependency);
    }
    return !refs.empty();
}

// 独立读回的同一任务只计一次，直接观察还须与记录的原值一致。
void check_claim(sqlite3* db,std::string_view kind,const std::string& statement,const std::vector<MemoryBasis>& sources) {
    if (kind=="observation" || kind=="rule") {
        std::set<std::string> distinct;
        for (const auto& source : sources) {
            if (source.kind!="tool_readback") throw std::invalid_argument("knowledge: independent readback required");
            distinct.insert(source.dependency);
        }
        if (kind=="rule" && distinct.size()<2) throw std::invalid_argument("knowledge: two independent observations required");
        if (kind=="observation") {
            Statement actual(db,"SELECT actual FROM task_observation WHERE task=?");
            actual.bind(1,sources.front().record);
            if (sources.size()!=1 || !actual.next() || actual.text(0)!=statement)
                throw std::invalid_argument("knowledge: observation must reproduce the verified value");
        }
    } else if (std::ranges::any_of(sources,[kind](const auto& s){
        return kind=="external" ? s.kind!="owner_statement" && s.kind!="external_document" : s.kind=="external_document";
    }))
        throw std::invalid_argument("knowledge: external source required");
}
}

// 让跨渠道比较与发布共用同一套原件核验。
std::optional<MemoryBasis> message_db::resolve_basis(sqlite3* db,const std::string& profile,const std::string& ref) {
    return basis(db,profile,ref);
}

// 保存库自己的说明、来源及支持检索的标签，库类别初次登记后固定。
void MessageStore::define_knowledge_library(const Json& definition) {
    const auto id=definition.at("library").get<std::string>(), category=definition.at("category").get<std::string>();
    const auto summary=definition.at("summary").get<std::string>(), origin=definition.at("origin").get<std::string>();
    const auto tags=definition.at("tags").get<std::vector<std::string>>();
    key(id); prose(summary,256); prose(origin,256);
    if ((category!="observed" && category!="external") || tags.size()>6 || std::set(tags.begin(),tags.end()).size()!=tags.size())
        throw std::invalid_argument("knowledge: invalid library category or tags");
    for (const auto& tag : tags) prose(tag,64);
    std::lock_guard lock(state_->mutex);
    Statement save(state_->db,"INSERT INTO knowledge_library VALUES(?,?,?,?,?) ON CONFLICT(library) "
        "DO UPDATE SET summary=excluded.summary,origin=excluded.origin,tags=excluded.tags WHERE category=excluded.category");
    save.bind(1,id); save.bind(2,category); save.bind(3,summary); save.bind(4,origin); save.bind(5,Json(tags).dump()); save.next();
    if (!sqlite3_changes(state_->db)) throw std::invalid_argument("knowledge: library category is fixed");
}

// 用同一登记信息给管理端返回主题、来历和标签。
Json MessageStore::knowledge_libraries(std::string_view library) const {
    if (!library.empty()) key(library);
    std::lock_guard lock(state_->mutex);
    Statement rows(state_->db,"SELECT library,category,summary,origin,tags FROM knowledge_library "
        "WHERE ?1='' OR library=?1 ORDER BY library LIMIT 65");
    rows.bind(1,library.empty() ? std::string_view("") : library);
    auto result=Json::array();
    while (rows.next()) {
        if (result.size()==64) return {{"libraries",std::move(result)},{"limited",true}};
        result.push_back({{"library",rows.text(0)},{"category",rows.text(1)},{"summary",rows.text(2)},
            {"origin",rows.text(3)},{"tags",Json::parse(rows.text(4))}});
    }
    return {{"libraries",std::move(result)},{"limited",false}};
}

// 提交一个独立编号的外部资料快照，重复导入只允许相同字节、标题与来源。
// 步骤：核对文本和标识 → 计算内容摘要 → 原子写入或核对已有版本。
Json MessageStore::import_knowledge_document(std::string_view id,std::string_view version,const std::string& title,
    const std::string& origin,const std::string& body) {
    key(id); key(version); prose(title,180); prose(origin,256);
    if (body.empty() || body.size()>16*1024 || !runtime::valid_utf8(body) || std::ranges::any_of(body,[](unsigned char c){
        return c<32 && c!='\r' && c!='\n' && c!='\t';
    })) throw std::invalid_argument("knowledge: expected UTF-8 text of at most 16 KiB");
    const auto ref=conversation()+"/document/"+std::string(id)+"/"+std::string(version), digest=platform::sha256(body);
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement existing(db,"SELECT title,origin,body,active FROM knowledge_document WHERE reference=?"); existing.bind(1,ref);
    if (existing.next()) {
        if (existing.text(0)!=title || existing.text(1)!=origin || existing.text(2)!=body || !existing.number(3))
            throw std::invalid_argument("knowledge: use a new document version");
    } else {
        Statement add(db,"INSERT INTO knowledge_document(reference,title,origin,sha256,body) VALUES(?,?,?,?,?)");
        add.bind(1,ref); add.bind(2,title); add.bind(3,origin); add.bind(4,digest); add.bind(5,body); add.next();
    }
    tx.commit(); return {{"reference",ref},{"sha256",digest},{"bytes",body.size()}};
}

// 将原件原样交回给主人；跨档案与撤下编号一律拒绝。
Json MessageStore::inspect_knowledge_document(const std::string& reference) const {
    std::lock_guard lock(state_->mutex);
    if (!reference.starts_with(conversation()+"/document/")) throw std::invalid_argument("knowledge: wrong document archive");
    Statement get(state_->db,"SELECT title,origin,sha256,body FROM knowledge_document WHERE reference=? AND active=1");
    get.bind(1,reference);
    if (!get.next()) throw std::invalid_argument("knowledge: unavailable document");
    return {{"reference",reference},{"title",get.text(0)},{"origin",get.text(1)},{"sha256",get.text(2)},{"text",get.text(3)}};
}

// 撤权和授权修订同事务生效，重试不会复活原件或再次推进修订。
bool MessageStore::withdraw_knowledge_document(const std::string& reference) {
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    if (!reference.starts_with(conversation()+"/document/")) throw std::invalid_argument("knowledge: wrong document archive");
    Statement get(db,"SELECT active FROM knowledge_document WHERE reference=?"); get.bind(1,reference);
    if (!get.next()) throw std::invalid_argument("knowledge: unknown document");
    const bool changed=get.number(0)!=0;
    if (changed) {
        Statement withdraw(db,"UPDATE knowledge_document SET active=0 WHERE reference=?"); withdraw.bind(1,reference); withdraw.next();
        sql(db,"UPDATE conversation SET memory_revision=memory_revision+1");
    }
    tx.commit(); return changed;
}

// 在幂等提议键下保存草案与逐项有类型的原件，已发布内容只可另建修订。
Json MessageStore::propose_knowledge(const Json& draft,std::string_view proposer) {
    const auto article=draft.at("article").get<std::string>(), tag=draft.at("edition").get<std::string>();
    const auto topic=draft.at("topic").get<std::string>(), scope=draft.at("applies").get<std::string>(), text=draft.at("statement").get<std::string>();
    const auto base=draft.at("base").get<std::int64_t>(); const auto refs=draft.at("sources").get<std::vector<std::string>>();
    const auto library=draft.value("library",std::string("observed")), kind=draft.value("kind",std::string("legacy"));
    key(article); key(tag); prose(topic,130); prose(scope,240); prose(text,640);
    key(library);
    if (base<0 || refs.empty() || refs.size()>3 || std::set<std::string>(refs.begin(),refs.end()).size()!=refs.size())
        throw std::invalid_argument("knowledge: invalid basis or revision");
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement catalog(db,"SELECT category FROM knowledge_library WHERE library=?"); catalog.bind(1,library);
    if (!catalog.next()) throw std::invalid_argument("knowledge: unknown library");
    const auto category=catalog.text(0);
    if ((category=="observed" && kind!="observation" && kind!="rule" &&
        !(kind=="legacy" && !draft.contains("kind") && !draft.contains("library"))) || (category=="external" && kind!="external"))
        throw std::invalid_argument("knowledge: claim kind does not belong to library");
    Statement make(db,"INSERT OR IGNORE INTO knowledge_article(article,library) VALUES(?,?)"); make.bind(1,article); make.bind(2,library); make.next();
    Statement current(db,"SELECT revision,library FROM knowledge_article WHERE article=?"); current.bind(1,article); current.next();
    if (current.text(1)!=library) throw std::invalid_argument("knowledge: article library is fixed");
    Statement prior(db,"SELECT id,base,topic,applies,statement,proposer,claim_kind FROM knowledge_edition WHERE article=? AND tag=?");
    prior.bind(1,article); prior.bind(2,tag);
    std::int64_t id=0;
    if (prior.next()) {
        id=prior.number(0);
        if(prior.number(1)!=base || prior.text(2)!=topic || prior.text(3)!=scope || prior.text(4)!=text || prior.text(5)!=proposer || prior.text(6)!=kind)
            throw std::invalid_argument("knowledge: conflicting proposal");
        Statement existing(db,"SELECT reference FROM knowledge_basis WHERE edition=? ORDER BY position"); existing.bind(1,id);
        for(const auto& ref : refs) if (!existing.next() || existing.text(0)!=ref) throw std::invalid_argument("knowledge: conflicting sources");
        if(existing.next()) throw std::invalid_argument("knowledge: conflicting sources");
    } else {
        if (current.number(0)!=base) throw std::invalid_argument("knowledge: stale base revision");
        if (kind=="legacy") {
            Statement typed(db,"SELECT 1 FROM knowledge_edition WHERE article=? AND claim_kind<>'legacy' LIMIT 1"); typed.bind(1,article);
            if (typed.next()) throw std::invalid_argument("knowledge: typed revision required");
        }
        Statement add(db,"INSERT INTO knowledge_edition(article,tag,base,topic,applies,statement,proposer,claim_kind) VALUES(?,?,?,?,?,?,?,?)");
        add.bind(1,article); add.bind(2,tag); add.bind(3,base); add.bind(4,topic); add.bind(5,scope); add.bind(6,text); add.bind(7,proposer);
        add.bind(8,kind); add.next();
        id=sqlite3_last_insert_rowid(db);
    }
    std::vector<MemoryBasis> sources;
    for(std::size_t i=0;i<refs.size();++i) {
        const auto source=basis(db,conversation(),refs[i]); if (!source) throw std::invalid_argument("knowledge: unavailable source");
        Statement add(db,"INSERT OR IGNORE INTO knowledge_basis VALUES(?,?,?,?,?)"); add.bind(1,id); add.bind(2,static_cast<std::int64_t>(i));
        add.bind(3,refs[i]); add.bind(4,source->kind); add.bind(5,source->dependency); add.next();
        sources.push_back(*source);
    }
    check_claim(db,kind,text,sources);
    tx.commit(); return {{"article",article},{"edition",tag},{"base",base},{"state","recorded"}};
}

// 把可用的独立读回原值登记为客观观察，不接受模型撰写的正文或范围。
// 步骤：核对库类别与原件 → 检查同源重试 → 同事务发布原值和出处。
Json MessageStore::record_knowledge_observation(const std::string& library,const std::string& reference) {
    key(library);
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement catalog(db,"SELECT 1 FROM knowledge_library WHERE library=? AND category='observed'"); catalog.bind(1,library);
    if (!catalog.next()) throw std::invalid_argument("knowledge: observations require an observed library");
    const auto source=basis(db,conversation(),reference);
    if (!source || source->kind!="tool_readback") throw std::invalid_argument("knowledge: independent readback required");
    const auto article="obs-"+platform::sha256(library+"\n"+reference).substr(0,24);
    Statement prior(db,"SELECT a.head,e.id FROM knowledge_article a LEFT JOIN knowledge_edition e "
        "ON e.article=a.article AND e.tag='readback' AND e.publisher='host' JOIN knowledge_library l USING(library) WHERE a.article=? AND l.library=?");
    prior.bind(1,article); prior.bind(2,library);
    if (prior.next()) {
        const auto edition=prior.number(1);
        Json refs=Json::array();
        if (!edition || !references(db,conversation(),edition,refs) || refs.size()!=1 || refs.front().at("reference")!=reference)
            throw std::invalid_argument("knowledge: conflicting observation identity");
        tx.commit();
        return {{"article",article},{"edition","readback"},{"state",prior.number(0)==edition ? "published" : "inactive"},{"created",false}};
    }

    Statement actual(db,"SELECT json_extract(family,'$[1]'),actual FROM task_observation WHERE task=?");
    actual.bind(1,source->record); if (!actual.next()) throw std::invalid_argument("knowledge: missing verified value");
    const auto reader=actual.text(0), observation=actual.text(1); prose(reader,130); prose(observation,640);
    Statement article_row(db,"INSERT INTO knowledge_article(article,library) VALUES(?,?)");
    article_row.bind(1,article); article_row.bind(2,library); article_row.next();
    Statement add(db,"INSERT INTO knowledge_edition(article,tag,base,topic,applies,statement,proposer,claim_kind,published,publisher) "
        "VALUES(?,'readback',0,?, '只记录此原件对应的一次独立现场读回',?,'host','observation',1,'host')");
    add.bind(1,article); add.bind(2,reader); add.bind(3,observation); add.next();
    const auto edition=sqlite3_last_insert_rowid(db);
    Statement head(db,"UPDATE knowledge_article SET revision=1,head=? WHERE article=?");
    head.bind(1,edition); head.bind(2,article); head.next();
    Statement cite(db,"INSERT INTO knowledge_basis VALUES(?,0,?,'tool_readback',?)");
    cite.bind(1,edition); cite.bind(2,reference); cite.bind(3,source->dependency); cite.next();
    sql(db,"UPDATE conversation SET memory_revision=memory_revision+1");
    tx.commit(); return {{"article",article},{"edition","readback"},{"state","published"},{"created",true}};
}

// 由主人发布当前基线上的候选或按当前修订撤下，历史版本永不自动回升。
bool MessageStore::publish_knowledge(const std::string& article,const std::string& tag,std::optional<std::int64_t> withdraw) {
    key(article); std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement get(db,"SELECT revision,head FROM knowledge_article WHERE article=?"); get.bind(1,article);
    if (!get.next()) throw std::invalid_argument("knowledge: unknown article");
    const auto rev=get.number(0), head=get.number(1); std::int64_t next=0;
    if (withdraw) {
        if (!head && rev>0 && *withdraw==rev-1) {tx.commit(); return false;}
        if (!head || *withdraw!=rev) throw std::invalid_argument("knowledge: stale withdrawal");
    } else {
        key(tag); Statement draft(db,"SELECT id,base,published FROM knowledge_edition WHERE article=? AND tag=?");
        draft.bind(1,article); draft.bind(2,tag); if (!draft.next()) throw std::invalid_argument("knowledge: unknown proposal");
        next=draft.number(0); Json refs=Json::array();
        if (!references(db,conversation(),next,refs)) throw std::invalid_argument("knowledge: withdrawn evidence");
        if (head==next) {tx.commit(); return false;}
        if (draft.number(1)!=rev || draft.number(2)) throw std::invalid_argument("knowledge: stale proposal");
        Statement publish(db,"UPDATE knowledge_edition SET published=? WHERE id=?"); publish.bind(1,rev+1); publish.bind(2,next); publish.next();
    }
    Statement update(db,"UPDATE knowledge_article SET revision=revision+1,head=? WHERE article=?"); update.bind(1,next); update.bind(2,article); update.next();
    sql(db,"UPDATE conversation SET memory_revision=memory_revision+1"); tx.commit(); return true;
}

// 主人只向实际已绑定的子库授权本版，后续修订重新确定受众。
std::int64_t MessageStore::grant_knowledge(const std::string& article,const std::string& tag,const std::string& plugin,
    const plugins::MemoryIdentity& who,int rights) {
    if(rights!=0 && rights!=1 && rights!=5) throw std::invalid_argument("knowledge: invalid grant");
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement target(db,"SELECT id FROM child_owner WHERE plugin=? AND preset=? AND subject=? AND channel=?");
    target.bind(1,plugin); target.bind(2,static_cast<unsigned>(who.preset)); target.bind(3,who.subject); target.bind(4,who.channel);
    if(!target.next()) throw std::invalid_argument("knowledge: unknown child");
    const auto child=target.number(0);
    Statement version(db,"SELECT e.id,a.head FROM knowledge_edition e JOIN knowledge_article a USING(article) WHERE e.article=? AND e.tag=?");
    version.bind(1,article); version.bind(2,tag); if(!version.next()) throw std::invalid_argument("knowledge: unknown edition");
    Json refs=Json::array();
    if(rights && (version.number(0)!=version.number(1) || !references(db,conversation(),version.number(0),refs)))
        throw std::invalid_argument("knowledge: unavailable publication");
    Statement set(db,"INSERT INTO knowledge_grant VALUES(?,?,?) ON CONFLICT(edition,child) DO UPDATE SET rights=excluded.rights WHERE rights<>excluded.rights");
    set.bind(1,version.number(0)); set.bind(2,child); set.bind(3,rights); set.next();
    const auto changed=sqlite3_changes(db); tx.commit(); return changed ? child : 0;
}

// 管理端检查当前版本、候选及其原件，已撤依据只留版本身份供审计。
Json MessageStore::inspect_knowledge(const std::string& article,const std::string& tag) const {
    key(article); std::lock_guard lock(state_->mutex); auto* db=state_->db;
    Statement get(db,"SELECT revision,head,library,category FROM knowledge_article JOIN knowledge_library USING(library) WHERE article=?"); get.bind(1,article);
    if(!get.next()) throw std::invalid_argument("knowledge: unknown article");
    Json result{{"article",article},{"revision",get.number(0)},{"library",get.text(2)},{"category",get.text(3)},{"editions",Json::array()}};
    Statement rows(db,"SELECT id,tag,base,published,topic,applies,statement,proposer,claim_kind,publisher FROM knowledge_edition "
        "WHERE article=? AND (?2='' OR tag=?2) ORDER BY id DESC LIMIT 16");
    rows.bind(1,article); rows.bind(2,tag);
    while(rows.next()) {
        Json item{{"edition",rows.text(1)},{"base",rows.number(2)},{"publishedRevision",rows.number(3)},{"current",rows.number(0)==get.number(1)},
            {"claimKind",rows.text(8)},{"publisher",rows.number(3) ? rows.text(9) : std::string{}}};
        if (!tag.empty()) {
            Json refs=Json::array();
            if (references(db,conversation(),rows.number(0),refs)) {
                item["topic"]=rows.text(4); item["applies"]=rows.text(5); item["statement"]=rows.text(6); item["proposer"]=rows.text(7); item["sources"]=refs;
            } else item["evidenceUnavailable"]=true;
        }
        result["editions"].push_back(std::move(item));
    }
    return result;
}

// 仅投影当前已发布且全部原件仍获准的版本。子库接收发布文本和来源类别，原件号保留给主人。
Json MessageStore::search_knowledge(const std::vector<std::string>& topics,unsigned count,std::int64_t child,bool outward,
    std::vector<std::string>* dependencies) const {
    if (topics.empty() || topics.size()>3 || !count) throw std::invalid_argument("knowledge: invalid query");
    for(const auto& topic : topics) prose(topic,160);
    std::lock_guard lock(state_->mutex); auto* db=state_->db;
    Statement hits(db,"WITH q(term) AS (VALUES(?1),(?2),(?3)) SELECT e.id,e.article,e.tag,e.topic,e.applies,e.statement,e.published,e.proposer,"
        "l.library,l.category,e.claim_kind,e.publisher FROM knowledge_article a JOIN knowledge_edition e ON e.id=a.head JOIN knowledge_library l USING(library) JOIN q "
        "WHERE term<>'' AND (a.article=term OR instr(e.topic,term)>0 OR l.library=term OR EXISTS(SELECT 1 FROM json_each(l.tags) WHERE value=term)) "
        "AND (?4=0 OR EXISTS(SELECT 1 FROM knowledge_grant g "
        "JOIN child_owner c ON c.id=g.child WHERE g.edition=e.id AND c.id=?4 AND (g.rights & ?5)=?5 AND (c.access & 1)=1 "
        "AND (c.preset<>3 OR (c.access & ?5)=?5))) GROUP BY e.id ORDER BY max(a.article=term) DESC,count(DISTINCT term) DESC,"
        "max(e.topic=term) DESC,e.id DESC");
    for(int i=0;i<3;++i) hits.bind(i+1,i<static_cast<int>(topics.size()) ? topics[i] : std::string{});
    hits.bind(4,child); hits.bind(5,outward ? 5 : 1);
    Json result{{"items",Json::array()},{"limited",false}};
    auto& items=result.at("items");
    while(hits.next()) {
        Json refs=Json::array(); std::vector<std::string> deps;
        if (!references(db,conversation(),hits.number(0),refs,&deps)) continue;
        if (items.size()>=count) {result["limited"]=true; break;}
        Json item{{"article",hits.text(1)},{"edition",hits.text(2)},{"topic",hits.text(3)},{"applies",hits.text(4)},{"statement",hits.text(5)},
            {"revision",hits.number(6)},{"proposer",hits.text(7)},{"publisher",hits.text(11)},{"sources",refs},
            {"library",hits.text(8)},{"category",hits.text(9)},{"claimKind",hits.text(10)}};
        if (child) for(auto& ref : item["sources"]) ref.erase("reference");
        items.push_back(std::move(item));
        if (items.dump().size()>2048) {items.erase(items.end()-1); result["limited"]=true; continue;}
        if(dependencies) dependencies->insert(dependencies->end(),deps.begin(),deps.end());
    }
    return result;
}
}
