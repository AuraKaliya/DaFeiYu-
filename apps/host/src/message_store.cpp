#include "message_store_db.hpp"
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <charconv>
#include <chrono>
#include <mutex>

namespace dfy::app {
using namespace message_db;
namespace {
using Json = nlohmann::json;

// 从共享列顺序解码一份持久消息和指定修订的回执。
StoredMessage row(const Statement& query) {
    return {query.number(0),query.number(1),query.number(2),query.text(3),query.text(4),
        Json::parse(query.text(5)),Json::parse(query.text(6))};
}
// 在当前锁或事务内查找原提交，消息体和最新回执总是来自同一次取样。
std::optional<StoredMessage> lookup(sqlite3* db, std::string_view source, std::string_view submission) {
    Statement query(db,"SELECT m.id,r.rev,m.utc,m.source,m.submission,m.envelope,r.body FROM messages m "
        "JOIN receipts r ON r.rev=m.latest WHERE m.source=? AND m.submission=?");
    query.bind(1,source);
    query.bind(2,submission);
    return query.next() ? std::optional(row(query)) : std::nullopt;
}
// 同步维护待办索引，崩溃恢复只扫描未能确定终态的少量消息。
void index_delivery(sqlite3* db, std::int64_t id, const Json& receipt) {
    const auto state = receipt.value("state",std::string{});
    const bool active = state=="pending" || state=="dispatching" || state=="waiting" || state=="queued";
    Statement update(db,active ? "INSERT OR IGNORE INTO outbox VALUES(?)" : "DELETE FROM outbox WHERE message=?");
    update.bind(1,id);
    update.next();
}
// 将新回执和消息的最新修订指针放入同一调用方事务。
std::int64_t append_receipt(sqlite3* db, std::int64_t id, const Json& receipt) {
    Statement add(db,"INSERT INTO receipts(message,body) VALUES(?,?)");
    add.bind(1,id);
    add.bind(2,receipt.dump());
    add.next();
    const auto revision = sqlite3_last_insert_rowid(db);
    Statement publish(db,"UPDATE messages SET latest=? WHERE id=?");
    publish.bind(1,revision);
    publish.bind(2,id);
    publish.next();
    index_delivery(db,id,receipt);
    return revision;
}
// 旧档案重建后也可能保留单个新列，升级时只补齐缺少的列。
bool has_column(sqlite3* db,std::string_view table,std::string_view name) {
    Statement columns(db,"SELECT 1 FROM pragma_table_info(?) WHERE name=?");
    columns.bind(1,table); columns.bind(2,name);
    return columns.next();
}
}


// 默认回滚日志和完整同步使原文接纳先于业务交付，逻辑会话号只在新库签发。
// 步骤：打开目录内的数据库 → 识别版本 → 原子建表与签发身份 → 读取固定身份。
MessageStore::MessageStore(const std::filesystem::path& profile) : state_(std::make_unique<State>()) {
    std::filesystem::create_directories(profile);
    const auto path = (profile/"main-conversation.sqlite3").u8string();
    check(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),&state_->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr));
    auto* db = state_->db;
    register_case_matching(db);
    check(sqlite3_busy_timeout(db,150));
    sql(db,"PRAGMA synchronous=FULL");
    Transaction transaction(db);
    {
        std::int64_t previous=0;
        {
            Statement version(db,"PRAGMA user_version");
            if (!version.next() || version.number(0)>25) throw std::runtime_error("messages: unsupported database version");
            previous=version.number(0);
        }
        if (previous==0) {
            sql(db,"CREATE TABLE conversation(id TEXT NOT NULL); INSERT INTO conversation VALUES(lower(hex(randomblob(16))));"
                "CREATE TABLE messages(id INTEGER PRIMARY KEY,source TEXT NOT NULL,submission TEXT NOT NULL,envelope TEXT NOT NULL,"
                "utc INTEGER NOT NULL,latest INTEGER NOT NULL DEFAULT 0,UNIQUE(source,submission));"
                "CREATE TABLE receipts(rev INTEGER PRIMARY KEY,message INTEGER NOT NULL,body TEXT NOT NULL);"
                "CREATE INDEX receipt_history ON receipts(message,rev); PRAGMA user_version=1");
        }
        if (previous<2) {
            sql(db,"CREATE TABLE outbox(message INTEGER PRIMARY KEY)");
            Statement existing(db,"SELECT m.id,r.body FROM messages m JOIN receipts r ON r.rev=m.latest");
            while (existing.next()) index_delivery(db,existing.number(0),Json::parse(existing.text(1)));
            sql(db,"PRAGMA user_version=2");
        }
        if (previous<3) sql(db,"ALTER TABLE conversation ADD COLUMN memory_revision INTEGER NOT NULL DEFAULT 0;"
            "CREATE TABLE personal_memory(message INTEGER PRIMARY KEY,topic TEXT NOT NULL,active INTEGER NOT NULL);"
            "CREATE INDEX active_memory_topic ON personal_memory(topic,message DESC) WHERE active=1; PRAGMA user_version=3");
        if (previous<4) sql(db,"ALTER TABLE personal_memory ADD COLUMN kind TEXT NOT NULL DEFAULT '';"
            "CREATE TABLE memory_review(message INTEGER PRIMARY KEY,series INTEGER NOT NULL,version INTEGER NOT NULL,"
            "basis INTEGER NOT NULL,excerpt TEXT NOT NULL,event_utc INTEGER NOT NULL);"
            "CREATE INDEX review_versions ON memory_review(series,version,message); PRAGMA user_version=4");
        if (previous<5) sql(db,"CREATE TABLE main_experience(message INTEGER PRIMARY KEY,parent INTEGER NOT NULL UNIQUE,topic TEXT NOT NULL,active INTEGER NOT NULL,open_offer INTEGER NOT NULL);"
            "CREATE INDEX experience_topic ON main_experience(topic,message DESC) WHERE active=1;"
            "CREATE TABLE experience_review(topic TEXT NOT NULL,kind TEXT NOT NULL,anchor INTEGER NOT NULL,later INTEGER NOT NULL,"
            "evidence TEXT NOT NULL,interpretation TEXT NOT NULL,PRIMARY KEY(topic,kind,anchor,later)); PRAGMA user_version=5");
        if (previous<6) sql(db,"CREATE TABLE task_memory(id INTEGER PRIMARY KEY,origin TEXT NOT NULL UNIQUE,goal TEXT NOT NULL,tool TEXT NOT NULL,"
            "body TEXT NOT NULL,active INTEGER NOT NULL DEFAULT 1,reason TEXT NOT NULL,signature TEXT NOT NULL,review TEXT NOT NULL DEFAULT '');"
            "CREATE INDEX task_review_queue ON task_memory(id) WHERE active=1 AND reason<>'' AND review='';"
            "CREATE INDEX task_transition ON task_memory(signature) WHERE active=1 AND signature<>''; PRAGMA user_version=6");
        if (previous<7) {
            sql(db,"CREATE TABLE task_observation(task INTEGER PRIMARY KEY,family TEXT NOT NULL,before_value TEXT NOT NULL,"
                "expected TEXT NOT NULL,actual TEXT NOT NULL,action INTEGER NOT NULL,baseline INTEGER NOT NULL,readback INTEGER NOT NULL,effect TEXT NOT NULL);"
                "CREATE INDEX observation_series ON task_observation(family,task);"
                "CREATE TABLE reflection_progress(family TEXT PRIMARY KEY,cursor INTEGER NOT NULL DEFAULT 0,head INTEGER NOT NULL DEFAULT 0);"
                "CREATE TABLE task_understanding(id INTEGER PRIMARY KEY,family TEXT NOT NULL,parent INTEGER NOT NULL,through_task INTEGER NOT NULL,"
                "relation TEXT NOT NULL,claim TEXT NOT NULL,cites TEXT NOT NULL,contrast INTEGER NOT NULL,valid INTEGER NOT NULL DEFAULT 1);"
                "CREATE INDEX understanding_series ON task_understanding(family,through_task); PRAGMA user_version=7");
            Statement old(db,"SELECT id,body FROM task_memory WHERE active=1 ORDER BY id");
            while (old.next()) index_task_observation(db,old.number(0),Json::parse(old.text(1)));
        }
        if (previous<8) {
            sql(db,"ALTER TABLE task_understanding ADD COLUMN recall_text TEXT NOT NULL DEFAULT ''; PRAGMA user_version=8");
            Statement rows(db,"SELECT id,claim FROM task_understanding WHERE valid=1");
            while (rows.next()) {
                Statement update(db,"UPDATE task_understanding SET recall_text=? WHERE id=?");
                update.bind(1,Json::parse(rows.text(1)).at("text").get<std::string>());
                update.bind(2,rows.number(0)); update.next();
            }
        }
        if (previous<9) sql(db,"CREATE TABLE IF NOT EXISTS child_owner(id INTEGER PRIMARY KEY,plugin TEXT NOT NULL,preset INTEGER NOT NULL,"
            "subject TEXT NOT NULL,channel TEXT NOT NULL,access INTEGER NOT NULL,source INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(plugin,preset,subject,channel));"
            "CREATE TABLE IF NOT EXISTS child_note(id INTEGER PRIMARY KEY,owner INTEGER NOT NULL,event TEXT NOT NULL,topic TEXT NOT NULL,"
            "content TEXT NOT NULL,source INTEGER NOT NULL,UNIQUE(owner,event));"
            "CREATE INDEX IF NOT EXISTS child_topic ON child_note(owner,topic,id DESC); PRAGMA user_version=9");
        if (previous<10) sql(db,"CREATE TABLE IF NOT EXISTS child_letter(id INTEGER PRIMARY KEY,owner INTEGER NOT NULL,"
            "exchange TEXT NOT NULL,phase TEXT NOT NULL,signature TEXT NOT NULL,envelope TEXT NOT NULL,source INTEGER NOT NULL,"
            "received INTEGER NOT NULL DEFAULT 0,seen INTEGER NOT NULL DEFAULT 0,UNIQUE(owner,exchange,phase));"
            "CREATE INDEX IF NOT EXISTS letter_pending ON child_letter(id) WHERE received=0;"
            "CREATE INDEX IF NOT EXISTS letter_inbox ON child_letter(id) WHERE received=1 AND seen=0; PRAGMA user_version=10");
        if (previous<11) sql(db,"CREATE TABLE IF NOT EXISTS knowledge_article(article TEXT PRIMARY KEY,revision INTEGER NOT NULL DEFAULT 0,head INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE IF NOT EXISTS knowledge_edition(id INTEGER PRIMARY KEY,article TEXT NOT NULL,tag TEXT NOT NULL,base INTEGER NOT NULL,"
            "topic TEXT NOT NULL,applies TEXT NOT NULL,statement TEXT NOT NULL,proposer TEXT NOT NULL,published INTEGER NOT NULL DEFAULT 0,"
            "UNIQUE(article,tag));"
            "CREATE TABLE IF NOT EXISTS knowledge_basis(edition INTEGER NOT NULL,position INTEGER NOT NULL,reference TEXT NOT NULL,"
            "kind TEXT NOT NULL,dependency TEXT NOT NULL,PRIMARY KEY(edition,position));"
            "CREATE TABLE IF NOT EXISTS knowledge_grant(edition INTEGER NOT NULL,child INTEGER NOT NULL,rights INTEGER NOT NULL,PRIMARY KEY(edition,child));"
            "PRAGMA user_version=11");
        if (previous<12) sql(db,"CREATE TABLE IF NOT EXISTS observation_case(name TEXT PRIMARY KEY,scope TEXT NOT NULL,cursor INTEGER NOT NULL DEFAULT 0,head INTEGER NOT NULL DEFAULT 0);"
            "CREATE TABLE IF NOT EXISTS case_member(id INTEGER PRIMARY KEY,name TEXT NOT NULL,checkpoint TEXT NOT NULL,reference TEXT NOT NULL UNIQUE,"
            "kind TEXT NOT NULL,record INTEGER NOT NULL,dependency TEXT NOT NULL,attribution TEXT NOT NULL,path TEXT NOT NULL,reader TEXT NOT NULL,"
            "reading TEXT NOT NULL,met INTEGER NOT NULL,active INTEGER NOT NULL DEFAULT 1);"
            "CREATE INDEX IF NOT EXISTS case_sequence ON case_member(name,id) WHERE active=1;"
            "CREATE INDEX IF NOT EXISTS case_dependency ON case_member(dependency) WHERE active=1;"
            "CREATE TABLE IF NOT EXISTS case_opinion(id INTEGER PRIMARY KEY,name TEXT NOT NULL,parent INTEGER NOT NULL,through_member INTEGER NOT NULL,"
            "judgement TEXT NOT NULL,body TEXT NOT NULL,cites TEXT NOT NULL,valid INTEGER NOT NULL DEFAULT 1); PRAGMA user_version=12");
        if (previous<13) sql(db,"CREATE TABLE IF NOT EXISTS letter_digest(letter INTEGER PRIMARY KEY,state TEXT NOT NULL,selection TEXT NOT NULL);"
            "CREATE INDEX IF NOT EXISTS digest_queue ON letter_digest(letter) WHERE state='pending'; PRAGMA user_version=13");
        if (previous<14) {
            sql(db,"CREATE TABLE IF NOT EXISTS case_term(name TEXT NOT NULL,kind TEXT NOT NULL,term TEXT NOT NULL COLLATE NOCASE,PRIMARY KEY(name,kind,term));"
                "CREATE INDEX IF NOT EXISTS case_checkpoint ON case_member(checkpoint COLLATE NOCASE,name) WHERE active=1; PRAGMA user_version=14");
            Statement cases(db,"SELECT name,scope FROM observation_case");
            while(cases.next()) index_case_terms(db,cases.text(0),Json::parse(cases.text(1)));
        }
        if(previous<15) sql(db,"CREATE INDEX IF NOT EXISTS case_native_checkpoint ON case_member(name,checkpoint,id) WHERE active=1 AND kind='tool_readback';"
            "CREATE INDEX IF NOT EXISTS case_kind_sequence ON case_member(name,kind,id) WHERE active=1; PRAGMA user_version=15");
        if(previous<16) sql(db,"CREATE TABLE IF NOT EXISTS idle_schedule(id INTEGER PRIMARY KEY CHECK(id=1),next_queue INTEGER NOT NULL CHECK(next_queue BETWEEN 0 AND 2));"
            "INSERT OR IGNORE INTO idle_schedule VALUES(1,0); PRAGMA user_version=16");
        // 已交付的旧信保留原样；开始和结束来源不一致时，停用相关报告和派生解释。
        if(previous<17) sql(db,"CREATE VIEW IF NOT EXISTS readable_letter AS SELECT e.* FROM child_letter e WHERE e.received=1 "
            "AND (e.source=0 OR EXISTS(SELECT 1 FROM personal_memory p WHERE p.message=e.source AND p.active=1)) "
            "AND (e.phase='start' OR EXISTS(SELECT 1 FROM child_letter s WHERE s.owner=e.owner AND s.exchange=e.exchange AND s.phase='start' AND s.source=e.source));"
            "CREATE TEMP TABLE invalid_member AS SELECT m.id,m.name FROM case_member m WHERE m.active=1 AND m.kind='sender_report' "
            "AND NOT EXISTS(SELECT 1 FROM readable_letter l WHERE l.id=m.record);"
            "UPDATE case_opinion SET valid=0 WHERE EXISTS(SELECT 1 FROM invalid_member m WHERE m.name=case_opinion.name AND m.id<=case_opinion.through_member);"
            "UPDATE observation_case SET cursor=0,head=CASE WHEN head IN(SELECT id FROM case_opinion WHERE valid=0) THEN 0 ELSE head END "
            "WHERE EXISTS(SELECT 1 FROM invalid_member m WHERE m.name=observation_case.name AND m.id<=observation_case.cursor);"
            "UPDATE case_member SET active=0 WHERE id IN(SELECT id FROM invalid_member); DROP TABLE invalid_member; PRAGMA user_version=17");
        if(previous<18) sql(db,"CREATE TABLE IF NOT EXISTS tone_correction(message INTEGER PRIMARY KEY,reply INTEGER NOT NULL,parent INTEGER NOT NULL,"
            "basis INTEGER NOT NULL,project TEXT NOT NULL,state TEXT NOT NULL,body TEXT NOT NULL,rewrite INTEGER NOT NULL DEFAULT 0,active INTEGER NOT NULL DEFAULT 1);"
            "CREATE INDEX IF NOT EXISTS tone_project ON tone_correction(project,message DESC) WHERE active=1 AND state='ready';"
            "CREATE INDEX IF NOT EXISTS tone_draft ON tone_correction(message) WHERE active=1 AND state='draft'; PRAGMA user_version=18");
        if(previous<19) sql(db,"CREATE TABLE IF NOT EXISTS recent_dialogue(slot INTEGER PRIMARY KEY CHECK(slot=1),"
            "through_message INTEGER NOT NULL,authority INTEGER NOT NULL,body TEXT NOT NULL,tone TEXT NOT NULL);"
            "CREATE INDEX IF NOT EXISTS recent_dialogue_reply ON messages(json_extract(envelope,'$.replyTo'),id) WHERE source='main';"
            "PRAGMA user_version=19");
        if(previous<20) {
            sql(db,"CREATE TABLE IF NOT EXISTS knowledge_library(library TEXT PRIMARY KEY,category TEXT NOT NULL CHECK(category IN ('observed','external')),"
                "summary TEXT NOT NULL,origin TEXT NOT NULL,tags TEXT NOT NULL);"
                "INSERT OR IGNORE INTO knowledge_library VALUES('observed','observed','从本体亲历、原件或已核验的读回整理','本档案的可读原件','[]'),"
                "('external','external','主人导入的客观资料','外部资料及其注明的出处','[]');");
            if (!has_column(db,"knowledge_article","library"))
                sql(db,"ALTER TABLE knowledge_article ADD COLUMN library TEXT NOT NULL DEFAULT 'observed'");
            if (!has_column(db,"knowledge_edition","claim_kind"))
                sql(db,"ALTER TABLE knowledge_edition ADD COLUMN claim_kind TEXT NOT NULL DEFAULT 'legacy'");
            sql(db,"PRAGMA user_version=20");
        }
        if(previous<21) sql(db,"CREATE TABLE IF NOT EXISTS knowledge_document(reference TEXT PRIMARY KEY,title TEXT NOT NULL,"
            "origin TEXT NOT NULL,sha256 TEXT NOT NULL,body TEXT NOT NULL,active INTEGER NOT NULL DEFAULT 1);"
            "PRAGMA user_version=21");
        if(previous<22) sql(db,"CREATE TABLE IF NOT EXISTS tone_scope_vector(slot INTEGER PRIMARY KEY CHECK(slot=1),"
            "scope TEXT NOT NULL,encoder TEXT NOT NULL,components TEXT NOT NULL); PRAGMA user_version=22");
        if(previous<23) {
            sql(db,"CREATE TABLE IF NOT EXISTS recent_dialogue_origin(message INTEGER PRIMARY KEY,parent INTEGER NOT NULL)");
            index_recent_origins(db);
            sql(db,"PRAGMA user_version=23");
        }
        if(previous<24) {
            if (!has_column(db,"knowledge_edition","publisher"))
                sql(db,"ALTER TABLE knowledge_edition ADD COLUMN publisher TEXT NOT NULL DEFAULT 'owner'");
            sql(db,"PRAGMA user_version=24");
        }
        if(previous<25) {
            if (!has_column(db,"knowledge_edition","verification"))
                sql(db,"ALTER TABLE knowledge_edition ADD COLUMN verification TEXT NOT NULL DEFAULT ''");
            sql(db,"CREATE INDEX IF NOT EXISTS checked_knowledge_family ON knowledge_edition(json_extract(verification,'$.family'),article) "
                "WHERE verification<>''; PRAGMA user_version=25");
        }
    }
    Statement identity(db,"SELECT id FROM conversation");
    if (!identity.next()) throw std::runtime_error("messages: missing conversation identity");
    const auto profile_id=identity.text(0);
    if (profile_id.size()!=32 || profile_id.find_first_not_of("0123456789abcdef")!=profile_id.npos || identity.next())
        throw std::runtime_error("messages: invalid or ambiguous conversation identity");
    state_->conversation = profile_id;
    transaction.commit();
}
// 归还数据库及其串行化状态。
MessageStore::~MessageStore() = default;
// 返回数据库给当前目录签发的逻辑身份。
const std::string& MessageStore::conversation() const noexcept { return state_->conversation; }

// 当次结论、延期或回退落盘后，下次启动先让另一类工作取得机会。
void message_db::complete_idle_queue(sqlite3* db,IdleQueue queue) {
    Statement save(db,"UPDATE idle_schedule SET next_queue=? WHERE id=1"); save.bind(1,static_cast<unsigned>(following_queue(queue))); save.next();
}
// 从本档案取已提交的调度进度，尚在途的提议不会改变它。
IdleQueue MessageStore::next_idle_queue() const {
    std::lock_guard lock(state_->mutex); Statement get(state_->db,"SELECT next_queue FROM idle_schedule WHERE id=1");
    if(!get.next()) throw std::runtime_error("memory: missing idle schedule");
    return static_cast<IdleQueue>(get.number(0));
}

// 管理终端核对实际打开的持久身份、数据结构版本和主聊天记忆修订。
nlohmann::json MessageStore::profile_info() const {
    std::lock_guard lock(state_->mutex); Statement version(state_->db,"PRAGMA user_version"); version.next();
    Statement info(state_->db,"SELECT memory_revision FROM conversation"); info.next();
    return {{"profile",conversation()},{"schemaVersion",version.number(0)},{"chatMemoryRevision",info.number(0)}};
}

namespace {
// 仅受信主聊天来源、真人原话及已接纳或实际送模的回执可作为个人记忆原件。
constexpr char memory_source[] = "SELECT m.id,m.envelope,r.body FROM messages m JOIN receipts r ON r.rev=m.latest "
    "WHERE m.source IN ('desktop','terminal')";
// 核对原渠道实际接纳状态和作者；补收录时使用与直接写入相同的正文计量。
bool eligible_original(const Statement& source, bool check_size) {
    const auto state = Json::parse(source.text(2)).at("state");
    if (state!="accepted" && state!="submitted") return false;
    const auto envelope = Json::parse(source.text(1));
    const auto text = envelope.find("text");
    return envelope.is_object() && envelope.value("sender",Json{})=="user" && text!=envelope.end() &&
        text->is_string() && (!check_size || text->dump().size()<=2048);
}
// 约束主题长度及命名空间，保留斜线给跨启动原件号。
void check_memory_topic(std::string_view topic) {
    if (topic.size()>128 || topic.find_first_not_of(" \t\r\n")==topic.npos ||
        topic.find('/')!=topic.npos || !runtime::valid_utf8(topic)) throw std::invalid_argument("memory: invalid topic");
}
}
// 向本体整理流程提供与现行记忆共用的原话接纳判据。
std::optional<std::string> message_db::owner_quote(sqlite3* db,std::int64_t id) {
    const auto query = std::string(memory_source)+" AND m.id=?";
    Statement source(db,query.c_str()); source.bind(1,id);
    if (!source.next() || !eligible_original(source,false)) return {};
    return Json::parse(source.text(1)).at("text").get<std::string>();
}
// 在当前档案内校验原件身份，拒绝跨档案或不完整的消息号。
std::int64_t message_db::memory_message(std::string_view identity, const std::string& conversation) {
    if (!identity.starts_with(conversation+"/")) throw std::invalid_argument("memory: different conversation");
    identity.remove_prefix(conversation.size()+1);
    std::int64_t id = 0;
    const auto result = std::from_chars(identity.data(),identity.data()+identity.size(),id);
    if (result.ec!=std::errc{} || result.ptr!=identity.data()+identity.size() || id<=0)
        throw std::invalid_argument("memory: invalid message identity");
    return id;
}
// 带显式主题的单行消息可在桌面或终端发出，实际保存完整原句。
std::optional<std::string_view> memory_capture_topic(std::string_view text) {
    constexpr std::string_view prefix = "记住 ", separator = "：";
    if (!text.starts_with(prefix)) return {};
    text.remove_prefix(prefix.size());
    const auto end = text.find(separator);
    if (end==text.npos || text.substr(end+separator.size()).find_first_not_of(" \t\r\n")==text.npos) return {};
    return text.substr(0,end);
}
// 读取本档案已提交的最近一次记忆权限变化。
std::int64_t MessageStore::memory_revision() const {
    std::lock_guard lock(state_->mutex);
    Statement current(state_->db,"SELECT memory_revision FROM conversation");
    current.next();
    return current.number(0);
}
// 取近期符合条件的真实输入，返回可以跨重启复用的完整原件号。
std::string MessageStore::last_memory_source() const {
    std::lock_guard lock(state_->mutex);
    Statement source(state_->db,(std::string(memory_source)+" ORDER BY m.id DESC").c_str());
    while (source.next()) if (eligible_original(source,false)) return conversation()+"/"+std::to_string(source.number(0));
    throw std::invalid_argument("memory: no accepted user message");
}
// 将记忆索引绑定不可变消息；撤权保留原件和墓碑，旧收录重试不能复活授权。
// 步骤：核对身份与原件 → 幂等改变索引 → 同一事务内提交权限修订。
MemoryChange MessageStore::change_memory(std::string_view identity, const std::optional<std::string>& topic,
    std::string_view excerpt, std::string_view kind) {
    const auto task_space = conversation()+"/task";
    if (identity.starts_with(task_space+"/")) {
        if (topic) throw std::invalid_argument("memory: task identity is already assigned");
        const auto id = memory_message(identity,task_space);
        std::lock_guard lock(state_->mutex);
        auto* db = state_->db;
        Transaction tx(db);
        Statement revoke(db,"UPDATE task_memory SET active=0,review='' WHERE active=1 AND id=?");
        revoke.bind(1,id); revoke.next();
        const bool changed = sqlite3_changes(db)!=0;
        Statement known(db,"SELECT 1 FROM task_memory WHERE id=?");
        known.bind(1,id);
        if (!known.next()) throw std::invalid_argument("memory: unknown task");
        if (changed) {
            revoke_case_basis(db,task_space+"/"+std::to_string(id));
            Statement invalidate(db,"UPDATE task_understanding SET valid=0 WHERE family=(SELECT family FROM task_observation WHERE task=?1) AND through_task>=?1");
            invalidate.bind(1,id); invalidate.next();
            Statement restart(db,"UPDATE reflection_progress SET cursor=0,head=0 WHERE family=(SELECT family FROM task_observation WHERE task=?1) AND cursor>=?1");
            restart.bind(1,id); restart.next();
            Statement remove(db,"DELETE FROM task_observation WHERE task=?");
            remove.bind(1,id); remove.next();
        }
        if (changed) sql(db,"UPDATE conversation SET memory_revision=memory_revision+1");
        Statement revision(db,"SELECT memory_revision FROM conversation");
        revision.next(); tx.commit();
        return {revision.number(0),changed,false,task_space+"/"+std::to_string(id)};
    }
    const auto id = memory_message(identity,conversation());
    if (topic) check_memory_topic(*topic);
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction transaction(db);
    Statement prior(db,"SELECT topic,active FROM personal_memory WHERE message=?");
    prior.bind(1,id);
    const bool exists = prior.next();
    bool changed = false;
    bool known_experience = false;
    bool known_tone = false;
    bool known_recent = false;
    if (!topic) {
        Statement known(db,"SELECT active FROM main_experience WHERE message=?1 OR parent=?1");
        known.bind(1,id);
        known_experience = known.next();
        if (known_experience && known.number(0)) {
            Statement revoke(db,"UPDATE main_experience SET active=0 WHERE message=?1 OR parent=?1");
            revoke.bind(1,id); revoke.next();
            changed = true;
        }
        Statement tone(db,"SELECT active FROM tone_correction WHERE message=?1 OR parent=?1 OR reply=?1 OR basis=?1 OR rewrite=?1 ORDER BY active DESC LIMIT 1");
        tone.bind(1,id);
        known_tone = tone.next();
        Statement recent(db,"SELECT 1 FROM recent_dialogue_origin WHERE message=?");
        recent.bind(1,id); known_recent=recent.next();
        if ((known_tone || known_recent) && !exists) {
            Statement tombstone(db,"INSERT INTO personal_memory(message,topic,active,kind) VALUES(?,'',0,?)");
            tombstone.bind(1,id); tombstone.bind(2,known_tone ? "tone" : "recent"); tombstone.next(); changed = true;
        }
        if (known_tone && tone.number(0)) {
            Statement revoke(db,"UPDATE tone_correction SET active=0 WHERE message=?1 OR parent=?1 OR reply=?1 OR basis=?1 OR rewrite=?1");
            revoke.bind(1,id); revoke.next(); changed = true;
        }
    }
    if (topic && (!exists || !excerpt.empty())) {
        const auto query = std::string(memory_source)+" AND m.id=?";
        Statement source(db,query.c_str());
        source.bind(1,id);
        if (!source.next() || !eligible_original(source,true)) throw std::invalid_argument("memory: eligible user quote within 2048 bytes required");
        if (!excerpt.empty() && Json::parse(source.text(1)).at("text").get<std::string>().find(excerpt)==std::string::npos)
            throw std::invalid_argument("memory: excerpt differs from original");
    }
    if (topic && !exists) {
        Statement add(db,"INSERT INTO personal_memory(message,topic,active,kind) VALUES(?,?,1,?)");
        add.bind(1,id); add.bind(2,*topic); add.bind(3,kind.empty() ? std::string_view("") : kind); add.next();
        changed = true;
    } else if ((!exists && !known_experience && !known_tone && !known_recent) || (topic && (prior.text(0)!=*topic || !prior.number(1)))) {
        throw std::invalid_argument("memory: unknown, revoked or conflicting original");
    } else if (!topic && exists && prior.number(1)) {
        Statement revoke(db,"UPDATE personal_memory SET active=0 WHERE message=?");
        revoke.bind(1,id); revoke.next();
        changed = true;
    }
    if (changed && !topic) revoke_case_basis(db,conversation()+"/"+std::to_string(id));
    if (changed) sql(db,topic ? "UPDATE conversation SET memory_revision=1 WHERE memory_revision=0" :
        "UPDATE conversation SET memory_revision=memory_revision+1");
    Statement revision(db,"SELECT memory_revision FROM conversation");
    revision.next();
    const auto result = revision.number(0);
    bool due = false;
    if (topic && !kind.empty()) {
        Statement related(db,"SELECT 1 FROM personal_memory WHERE active=1 AND topic=? AND message<? "
            "AND NOT EXISTS(SELECT 1 FROM memory_review WHERE message=?) LIMIT 1");
        related.bind(1,*topic); related.bind(2,id); related.bind(3,id);
        due = related.next();
    }
    transaction.commit();
    return {result,changed,due,conversation()+"/"+std::to_string(id)};
}
// 在同一事务核对双方原件，只允许沿当前版本增加依据或追加明确更正。
// 步骤：验证次序和逐字依据 → 检查重试与版本头 → 原子登记来源及它所支持的修订。
void MessageStore::reflect_memory(const Json& comparison) {
    const auto prior = memory_message(comparison.at("prior").get<std::string>(),conversation());
    const auto next = memory_message(comparison.at("later").get<std::string>(),conversation());
    const bool replace = comparison.at("relation")=="replaces";
    if (prior>=next) throw std::invalid_argument("memory: evidence order");
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction tx(db);
    std::string topic;
    std::int64_t series = prior, version = prior, prior_utc = 0;
    for (const auto id : {prior,next}) {
        const auto excerpt = comparison.at(id==prior ? "priorExcerpt" : "laterExcerpt").get<std::string>();
        Statement source(db,"SELECT p.topic,p.kind,m.envelope,m.utc,r.body FROM personal_memory p JOIN messages m ON m.id=p.message "
            "JOIN receipts r ON r.rev=m.latest WHERE p.message=? AND p.active=1 AND m.source IN ('desktop','terminal')");
        source.bind(1,id);
        if (!source.next()) throw std::invalid_argument("memory: unavailable original");
        const auto envelope = Json::parse(source.text(2));
        const auto state = Json::parse(source.text(4)).at("state");
        const auto utc = envelope.value("sourceUtcMs",source.number(3));
        if ((state!="accepted" && state!="submitted") || envelope.at("sender")!="user" ||
            excerpt.find_first_not_of(" \r\n\t")==excerpt.npos || excerpt.size()>384 ||
            envelope.at("text").get<std::string>().find(excerpt)==std::string::npos)
            throw std::invalid_argument("memory: unverified excerpt");
        if (id==prior) { topic = source.text(0); prior_utc = utc; }
        else if (utc<prior_utc || source.text(0)!=topic || (replace && !source.text(1).empty() && source.text(1)!="correction"))
            throw std::invalid_argument("memory: different topic or unconfirmed correction");
        Statement existing(db,"SELECT series,version,basis,excerpt FROM memory_review WHERE message=?");
        existing.bind(1,id);
        if (existing.next()) {
            if (id==prior) {
                if (existing.text(3)!=excerpt) throw std::invalid_argument("memory: different reviewed claim");
                series = existing.number(0); version = existing.number(1);
            }
            else if (existing.number(0)!=series || existing.number(1)!=(replace ? next : version) ||
                existing.number(2)!=prior || existing.text(3)!=excerpt) throw std::invalid_argument("memory: conflicting review");
            continue;
        }
        if (id==next) {
            Statement head(db,"SELECT max(CASE WHEN version<=?2 THEN version END),max(message),max(event_utc) "
                "FROM memory_review WHERE series=?1");
            head.bind(1,series); head.bind(2,next); head.next();
            if (head.number(0)!=version || (replace && (head.number(1)>=next || head.number(2)>utc)))
                throw std::invalid_argument("memory: stale comparison");
        }
        Statement insert(db,"INSERT INTO memory_review(message,series,version,basis,excerpt,event_utc) VALUES(?,?,?,?,?,?)");
        insert.bind(1,id); insert.bind(2,series); insert.bind(3,id==next && replace ? next : version);
        insert.bind(4,id==prior ? 0 : prior); insert.bind(5,excerpt); insert.bind(6,utc); insert.next();
    }
    tx.commit();
}
// 从获准索引回到同一份消息正文及渠道来源；同主题取最近的原消息。
std::optional<StoredMessage> MessageStore::recall_memory(std::string_view selector) const {
    const bool by_id = selector.find('/')!=selector.npos;
    if (by_id && !selector.starts_with(conversation()+"/")) return {};
    std::int64_t id = 0;
    if (by_id) {
        try { id = memory_message(selector,conversation()); } catch (const std::invalid_argument&) { return {}; }
    }
    std::lock_guard lock(state_->mutex);
    const std::string select = "SELECT m.id,r.rev,m.utc,m.source,m.submission,m.envelope,r.body FROM personal_memory p "
        "JOIN messages m ON m.id=p.message JOIN receipts r ON r.rev=m.latest WHERE p.active=1 AND ";
    Statement result(state_->db,(select+(by_id ? "p.message=?" : "p.topic=? AND NOT EXISTS(SELECT 1 FROM memory_review v "
        "WHERE v.message=p.message AND v.version<(SELECT max(version) FROM memory_review WHERE series=v.series)) "
        "ORDER BY p.message DESC LIMIT 1")).c_str());
    if (by_id) result.bind(1,id); else result.bind(1,selector);
    return result.next() ? std::optional(row(result)) : std::nullopt;
}
// 按完整主题、局部主题及新近次序取唯一原件，预算不足时继续尝试较短候选。
// 步骤：验证有限主题 → 在授权索引中合并排名 → 按序核算带来源的证据数组。
nlohmann::json MessageStore::search_memory(const std::vector<std::string>& topics, unsigned count, std::string_view view) const {
    if (topics.empty() || topics.size()>3 || !count) throw std::invalid_argument("memory: invalid search");
    std::lock_guard lock(state_->mutex);
    // 按版本整理前先取可读依据。版本头保留已撤回修订的位置，避免撤权后把旧偏好重新当作当前。
    Statement matches(state_->db,R"sql(
        WITH input(q) AS (VALUES(?1),(?2),(?3)),
        terms(term,pick) AS (
            SELECT q,0 FROM input WHERE typeof(q)='text'
            UNION ALL
            SELECT p.topic,p.message FROM input JOIN personal_memory p ON p.message=q
            WHERE typeof(q)='integer' AND p.active=1
        ), matched AS (
            SELECT p.message,p.topic,min(CASE WHEN p.message=t.pick THEN -1 WHEN p.topic=t.term THEN 0 ELSE 1 END) AS rank,
                   max(t.pick=0 OR t.pick=p.message) AS wanted
            FROM personal_memory p JOIN terms t ON (t.pick=0 AND instr(p.topic,t.term)>0) OR (t.pick>0 AND p.topic=t.term)
            WHERE p.active=1 GROUP BY p.message
        ), visible AS (
            SELECT p.*,v.version,v.excerpt,(SELECT max(version) FROM memory_review WHERE series=v.series) AS head,
                   min(p.message) OVER part AS first,max(p.message) OVER part AS last,count(*) OVER part AS mentions
            FROM matched p LEFT JOIN memory_review v ON v.message=p.message
            WINDOW part AS (PARTITION BY coalesce(v.version,p.message))
        ), ranked AS (
            SELECT *,CASE WHEN ?4='originals' THEN message ELSE last END AS recency FROM visible
            WHERE wanted=1 AND (?4='originals' OR version IS NULL OR (message=first AND (?4='history' OR version=head)))
            ORDER BY rank,recency DESC LIMIT 13
        )
        SELECT m.id,r.topic,m.source,m.utc,m.envelope,coalesce(r.version,0),r.excerpt,r.version=r.head,r.mentions,r.last,
               EXISTS(SELECT 1 FROM personal_memory u WHERE u.topic=r.topic AND u.active=1 AND u.kind='correction'
                      AND u.message>r.version AND NOT EXISTS(SELECT 1 FROM memory_review c WHERE c.message=u.message)),
               (SELECT old.message FROM memory_review anchor JOIN personal_memory old ON old.message=anchor.basis
                WHERE anchor.message=r.version AND old.active=1),
               CASE WHEN r.version IS NULL AND EXISTS(SELECT 1 FROM personal_memory earlier WHERE earlier.active=1
                      AND earlier.topic=r.topic AND earlier.message<r.message)
                    THEN CASE WHEN p.kind='correction' THEN 'correction' ELSE 'compare' END ELSE '' END
        FROM ranked r JOIN messages m ON m.id=r.message JOIN personal_memory p ON p.message=r.message
        ORDER BY r.rank,r.recency DESC
    )sql");
    for (std::size_t i=0;i<topics.size();++i) {
        const int slot = static_cast<int>(i+1);
        if (view=="originals" && topics[i].find('/')!=std::string::npos) matches.bind(slot,memory_message(topics[i],conversation()));
        else { check_memory_topic(topics[i]); matches.bind(slot,topics[i]); }
    }
    matches.bind(4,view);
    Json items = Json::array();
    std::size_t bytes = 2;
    bool limited = false;
    std::string review_due;
    unsigned candidates = 0;
    while (matches.next()) {
        if (++candidates>12 || items.size()>=count || items.size()>=3) { limited = true; break; }
        const auto envelope = Json::parse(matches.text(4));
        Json item{{"reference",conversation()+"/"+std::to_string(matches.number(0))},{"topic",matches.text(1)},
            {"source",matches.text(2)},{"utcMs",envelope.value("sourceUtcMs",matches.number(3))},{"quote",envelope.at("text")}};
        if (matches.number(5)) {
            item["standing"] = !matches.number(7) ? "superseded" : matches.number(10) ? "needs_review" : "current";
            item["conclusion"] = matches.text(6);
            item["mentions"] = matches.number(8);
            item["latestReference"] = conversation()+"/"+std::to_string(matches.number(9));
            if (matches.number(11)) item["previousReference"] = conversation()+"/"+std::to_string(matches.number(11));
        }
        const auto cost = item.dump().size() + (items.empty() ? 0 : 1);
        if (bytes+cost>2048) { limited = true; continue; }
        const auto due = matches.text(12);
        if (due=="correction" || review_due.empty()) review_due = due;
        bytes += cost;
        items.push_back(std::move(item));
    }
    Json selection{{"items",std::move(items)},{"limited",limited}};
    if (!review_due.empty()) selection["reviewDue"] = review_due;
    return selection;
}

// 从当前仍可读的经历生成主题索引，只交付计数和最近的原号。
// 步骤：核对主题范围 → 按精确程度和新近程度选择主题 → 逐组签发可查回的原件号。
Json MessageStore::search_memory_themes(const std::vector<std::string>& topics,unsigned count,std::string_view prompt) const {
    if (topics.size()>3 || !count || count>3) throw std::invalid_argument("memory: invalid theme search");
    for (const auto& topic : topics) check_memory_topic(topic);
    std::lock_guard lock(state_->mutex); auto* db=state_->db;
    Statement groups(db,"WITH terms(q) AS (VALUES(?1),(?2),(?3)) SELECT topic,count(*) AS mentions,max(message) AS latest "
        "FROM personal_memory WHERE active=1 AND kind IN ('','event') AND topic<>'' AND "
        "(CASE WHEN ?4<>'' THEN instr(?4,topic)>0 AND length(CAST(topic AS BLOB))>=6 "
        "WHEN ?1='' THEN 1 ELSE EXISTS(SELECT 1 FROM terms WHERE q<>'' AND instr(topic,q)>0) END) "
        "GROUP BY topic ORDER BY (topic IN (SELECT q FROM terms)) DESC,latest DESC LIMIT ?5");
    for (unsigned i=0;i<3;++i) groups.bind(i+1,i<topics.size() ? topics[i] : std::string{});
    groups.bind(4,prompt); groups.bind(5,count+1);
    Json items=Json::array(); bool limited=false;
    while (groups.next()) {
        if (items.size()==count) {limited=true; break;}
        const auto topic=groups.text(0);
        Statement notes(db,"SELECT message FROM personal_memory WHERE topic=? AND active=1 AND kind IN ('','event') ORDER BY message DESC LIMIT 3");
        notes.bind(1,topic);
        Json refs=Json::array();
        while (notes.next()) refs.push_back(conversation()+"/"+std::to_string(notes.number(0)));
        items.push_back({{"topic",topic},{"mentions",groups.number(1)},{"references",std::move(refs)}});
        if (items.dump().size()>2048) {items.erase(items.end()-1); limited=true; break;}
    }
    return {{"items",std::move(items)},{"limited",limited}};
}
namespace {
// 从同一份已送达的本体发言取事实与稳定身份，授权墓碑同时遮住原件和派生判断。
std::optional<Json> episode(sqlite3* db, const std::string& profile, std::int64_t id) {
    Statement row(db,"SELECT e.parent,m.utc,m.envelope,r.body,e.open_offer,p.envelope FROM main_experience e JOIN messages m ON m.id=e.message "
        "JOIN messages p ON p.id=e.parent JOIN receipts r ON r.rev=m.latest WHERE e.active=1 AND e.message=? AND m.source='main'");
    row.bind(1,id);
    if (!row.next() || Json::parse(row.text(3)).at("state")!="delivered") return {};
    const auto envelope = Json::parse(row.text(2));
    auto value = envelope.at("experience");
    value["reference"] = profile+"/"+std::to_string(id);
    value["parentReference"] = profile+"/"+std::to_string(row.number(0));
    value["utcMs"] = row.number(1);
    value["deliveredText"] = envelope.at("text");
    value["openOffer"] = row.number(4)!=0;
    const auto prompt = Json::parse(row.text(5)).at("text");
    if (prompt.dump().size()<=384) value["situationText"] = prompt;
    return value;
}
// 对可独立识别的主人直接委托保留凭据；其他自然措辞仍可记录亲历与事后解释。
bool explicit_offer(std::string_view input, std::string_view authority) {
    constexpr std::string_view invitations[]{"这次由你选","由你选","你来选","你自己选","你来决定","由你决定","你自己决定",
        "你想选哪个","你选哪一个","任选其一","都可以，你选","You decide","It's your choice"};
    if (std::ranges::find(invitations,authority)==std::end(invitations)) return false;
    const auto end = input.find_last_not_of(" \r\n\t");
    input = input.substr(0,end+1);
    for (std::string_view mark : {"。","？","！",".","?","!"})
        if (input.ends_with(mark)) { input.remove_suffix(mark.size()); break; }
    if (!input.ends_with(authority)) return false;
    input.remove_suffix(authority.size());
    for (std::string_view separator : {"。","？","！","，","；","：",", ",". ","\n"})
        if (input.ends_with(separator)) return true;
    return input.empty();
}
// 在实际发言的写事务内核对来信、授权、选项和逐字情境，每封来信只产生一次选择。
bool index_choice(sqlite3* db, std::int64_t message, const Json& envelope, const std::string& profile) {
    const auto& args = envelope.at("experience");
    const auto parent = memory_message(profile+"/"+envelope.at("replyTo").get<std::string>(),profile);
    Statement source(db,"SELECT m.envelope,r.body FROM messages m JOIN receipts r ON r.rev=m.latest WHERE m.id=?1 "
        "AND m.source IN ('desktop','terminal') AND NOT EXISTS(SELECT 1 FROM personal_memory WHERE message=?1 AND active=0) "
        "AND NOT EXISTS(SELECT 1 FROM main_experience WHERE parent=?1)");
    source.bind(1,parent);
    if (!source.next()) throw std::invalid_argument("experience: unavailable input");
    const auto input = Json::parse(source.text(0)), receipt = Json::parse(source.text(1));
    if (input.at("sender")!="user" || (receipt.at("state")!="accepted" && receipt.at("state")!="submitted") ||
        receipt.at("instance")!=envelope.at("instance") || receipt.at("session")!=envelope.at("session") || receipt.at("turn")!=envelope.at("turn"))
        throw std::invalid_argument("experience: wrong turn");
    const auto& text = input.at("text").get_ref<const std::string&>();
    const auto topic = args.at("topic").get<std::string>();
    check_memory_topic(topic);
    // 所有描述主人提供的条件与选项都须能在原信逐字定位。
    const auto excerpt = [&](const std::string& value, std::size_t limit) {
        return value.size()<=limit && value.find_first_not_of(" \r\n\t")!=value.npos && text.find(value)!=text.npos;
    };
    auto options = args.at("options").get<std::vector<std::string>>();
    std::ranges::sort(options);
    if (!excerpt(args.at("scene"),160) || !excerpt(args.at("authority"),160) || options.empty() || options.size()>4 ||
        (args.at("agency")=="free" && options.size()<2) || std::adjacent_find(options.begin(),options.end())!=options.end() ||
        std::ranges::any_of(options,[&](const auto& option){return !excerpt(option,64);}) ||
        !std::ranges::binary_search(options,args.at("choice").get<std::string>()) ||
        args.at("impression").get_ref<const std::string&>().size()>192)
        throw std::invalid_argument("experience: unsupported choice");
    Statement prior(db,"SELECT 1 FROM main_experience WHERE topic=? AND active=1 LIMIT 1");
    prior.bind(1,topic);
    const bool due = prior.next();
    Statement add(db,"INSERT INTO main_experience(message,parent,topic,active,open_offer) VALUES(?,?,?,1,?)");
    add.bind(1,message); add.bind(2,parent); add.bind(3,topic);
    add.bind(4,args.at("agency")=="free" && explicit_offer(text,args.at("authority").get_ref<const std::string&>())); add.next();
    sql(db,"UPDATE conversation SET memory_revision=1 WHERE memory_revision=0");
    return due;
}
}

// 查原件时排序和限量；查结论时把列明的依据作为一组，在同一预算内全部交付。
Json MessageStore::search_experience(const std::vector<std::string>& topics, unsigned count, std::string_view view) const {
    if (topics.empty() || topics.size()>3 || !count || (view!="episodes" && topics.size()!=1))
        throw std::invalid_argument("experience: invalid query");
    auto* db = state_->db;
    std::lock_guard lock(state_->mutex);
    Json result{{"items",Json::array()},{"limited",false}};
    if (view=="episodes") {
        Statement rows(db,"WITH q(term) AS (VALUES(?1),(?2),(?3)) SELECT message FROM main_experience JOIN q ON "
            "(typeof(term)='integer' AND term=message) OR (typeof(term)='text' AND instr(topic,term)>0) WHERE active=1 "
            "GROUP BY message ORDER BY min(CASE WHEN term=message THEN 0 WHEN term=topic THEN 1 ELSE 2 END),message DESC LIMIT 13");
        for (std::size_t i=0;i<topics.size();++i) {
            const int slot = static_cast<int>(i+1);
            if (topics[i].find('/')!=std::string::npos) rows.bind(slot,memory_message(topics[i],conversation()));
            else { check_memory_topic(topics[i]); rows.bind(slot,topics[i]); }
        }
        auto& items = result.at("items");
        while (rows.next()) {
            if (items.size()>=count) { result["limited"] = true; break; }
            if (auto item = episode(db,conversation(),rows.number(0))) {
                items.push_back(std::move(*item));
                if (items.dump().size()>2048) { items.erase(items.end()-1); result["limited"] = true; }
            }
        }
        return result;
    }
    Statement review(db,"SELECT evidence,interpretation FROM experience_review WHERE kind=?1 AND "
        "((typeof(?2)='text' AND topic=?2) OR (typeof(?2)='integer' AND (anchor=?2 OR later=?2))) "
        "ORDER BY (anchor=?2) DESC,later DESC,anchor DESC LIMIT 1");
    review.bind(1,view);
    if (topics[0].find('/')!=std::string::npos) review.bind(2,memory_message(topics[0],conversation()));
    else { check_memory_topic(topics[0]); review.bind(2,topics[0]); }
    if (!review.next()) return result;
    auto items = Json::array();
    for (const auto id : Json::parse(review.text(0)).get<std::vector<std::int64_t>>()) {
        auto item = episode(db,conversation(),id);
        if (!item) return result;
        items.push_back(std::move(*item));
    }
    Json complete{{"items",std::move(items)},{"limited",false},{"kind",view}};
    if (view=="why") complete["interpretation"] = review.text(1);
    if (complete.at("items").size()<=count) {
        if (complete.dump().size()<=2048) return complete;
        // 原场次可以逐份展开，概要优先保留每次实选及原件号。
        for (auto& item : complete.at("items"))
            for (const auto field : {"deliveredText","options","authority","topic","parentReference","impression","situationText"}) item.erase(field);
        complete["limited"] = true;
        if (complete.dump().size()<=2048) return complete;
    }
    result["limited"] = true;
    return result;
}

// 以原件号固定解释的支持集；选择变化额外核实自主性、相同条件、相同可选项及不同实收。
// 步骤：核实独立经历和先后 → 检查判断种类 → 在各自分支幂等追加修订。
void MessageStore::review_experience(const Json& review) {
    const auto refs = review.at("evidence").get<std::vector<std::string>>();
    const auto kind = review.at("kind").get<std::string>(), why = review.at("interpretation").get<std::string>();
    if (refs.size()<2 || refs.size()>3 || (kind!="why" && kind!="change") || why.size()>384 ||
        (kind=="why" ? why.find_first_not_of(" \r\n\t")==why.npos : !why.empty()))
        throw std::invalid_argument("experience: invalid review");
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction tx(db);
    std::vector<std::int64_t> ids;
    Json first;
    for (const auto& ref : refs) {
        const auto id = memory_message(ref,conversation());
        auto item = episode(db,conversation(),id);
        if (!item || (!ids.empty() && (id<=ids.back() || item->at("topic")!=first.at("topic"))))
            throw std::invalid_argument("experience: unordered or unrelated sources");
        auto& options = item->at("options");
        std::sort(options.begin(),options.end());
        if (ids.empty()) first = *item;
        if (kind=="change" && (item->at("openOffer")!=true || item->at("scene")!=first.at("scene") ||
            options!=first.at("options") || (ids.size()+1==refs.size() && item->at("choice")==first.at("choice"))))
            throw std::invalid_argument("experience: choices not comparable");
        ids.push_back(id);
    }
    const auto topic = first.at("topic").get<std::string>(), evidence = Json(ids).dump();
    Statement existing(db,"SELECT later,evidence,interpretation FROM experience_review WHERE topic=? AND kind=? AND anchor=? AND later>=? ORDER BY later LIMIT 1");
    existing.bind(1,topic); existing.bind(2,kind); existing.bind(3,ids.front()); existing.bind(4,ids.back());
    if (existing.next()) {
        if (existing.number(0)!=ids.back() || existing.text(1)!=evidence || existing.text(2)!=why)
            throw std::invalid_argument("experience: stale or conflicting review");
    } else {
        Statement add(db,"INSERT INTO experience_review(topic,kind,anchor,later,evidence,interpretation) VALUES(?,?,?,?,?,?)");
        add.bind(1,topic); add.bind(2,kind); add.bind(3,ids.front()); add.bind(4,ids.back()); add.bind(5,evidence); add.bind(6,why); add.next();
    }
    tx.commit();
}
// 用读锁固定当前提交位置。
std::optional<StoredMessage> MessageStore::find(std::string_view source, std::string_view submission) const {
    std::lock_guard lock(state_->mutex);
    return lookup(state_->db,source,submission);
}
// 按已经入库的消息号读取唯一有效的最新回执。
Json MessageStore::receipt(std::int64_t message) const {
    std::lock_guard lock(state_->mutex);
    Statement current(state_->db,"SELECT body FROM receipts WHERE rev=(SELECT latest FROM messages WHERE id=?)");
    current.bind(1,message);
    if (!current.next()) throw std::invalid_argument("messages: unknown message");
    return Json::parse(current.text(0));
}
// 写入用户原话或模型发言时把初始投递状态与原文一起提交。
// 步骤：核对原身份和载荷 → 插入首次消息 → 追加回执 → 写盘后交还身份。
MessageAdmission MessageStore::put(std::string_view source, std::string_view submission, const Json& envelope, const Json& receipt) {
    if (source.empty() || source.size()>32 || submission.empty() || submission.size()>160)
        throw std::invalid_argument("messages: invalid submission identity");
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction transaction(db);
    if (auto previous = lookup(db,source,submission)) {
        if (previous->envelope!=envelope) throw std::invalid_argument("messages: submission conflict");
        transaction.commit();
        return {std::move(*previous),false};
    }
    using namespace std::chrono;
    const auto utc = duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
    Statement insert(db,"INSERT INTO messages(source,submission,envelope,utc) VALUES(?,?,?,?)");
    insert.bind(1,source);
    insert.bind(2,submission);
    insert.bind(3,envelope.dump());
    insert.bind(4,utc);
    insert.next();
    const auto id = sqlite3_last_insert_rowid(db);
    auto actual = receipt;
    if (source=="main" && envelope.contains("experience") && actual.at("state")=="delivered" &&
        index_choice(db,id,envelope,conversation())) actual["reviewDue"] = "compare_experiences";
    const auto revision = append_receipt(db,id,actual);
    if (source=="main" && actual.value("state",Json{})=="delivered" && envelope.value("sender",Json{})=="assistant" &&
        envelope.contains("replyTo") && envelope.at("replyTo").is_string())
        index_recent_reply(db,id,envelope.at("replyTo").get_ref<const std::string&>());
    if ((source=="desktop" || source=="terminal") && envelope.is_object() && envelope.contains("text") &&
        envelope.at("text").is_string() && envelope.value("sender",Json{})=="user" && envelope.value("route",Json{})=="send" &&
        receipt.value("state",Json{})=="pending") {
        const auto& text = envelope.at("text");
        if (const auto topic = memory_capture_topic(text.get_ref<const std::string&>())) {
            check_memory_topic(*topic);
            if (text.dump().size()>2048) throw std::invalid_argument("memory: quote exceeds 2048 bytes");
            Statement retain(db,"INSERT INTO personal_memory(message,topic,active) VALUES(?,?,1)");
            retain.bind(1,id); retain.bind(2,*topic); retain.next();
            sql(db,"UPDATE conversation SET memory_revision=1 WHERE memory_revision=0");
        }
    }
    transaction.commit();
    return {{id,revision,utc,std::string(source),std::string(submission),envelope,std::move(actual)},true};
}
// 同一回执重试保留原序号，更新失败不发布水位。
void MessageStore::update(std::int64_t message, const Json& receipt) {
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction transaction(db);
    Statement current(db,"SELECT body FROM receipts WHERE rev=(SELECT latest FROM messages WHERE id=?)");
    current.bind(1,message);
    if (!current.next()) throw std::invalid_argument("messages: unknown message");
    if (Json::parse(current.text(0))!=receipt) append_receipt(db,message,receipt);
    transaction.commit();
}
// 只返回已经成功提交的修订上界。
std::int64_t MessageStore::watermark() const {
    std::lock_guard lock(state_->mutex);
    Statement query(state_->db,"SELECT coalesce(max(rev),0) FROM receipts");
    query.next();
    return query.number(0);
}

// 分页时同时约束记录数与正文大小，历史行使用指定水位以内的最后一份回执。
// 步骤：固定查询边界 → 按方向选择索引和修订 → 在预算内拷贝并探测下一页。
MessagePage MessageStore::page(bool history, std::int64_t cursor, std::int64_t through, unsigned limit) const {
    if (limit==0 || limit>64 || cursor<0 || through<0 || (!history && cursor>through) || through>watermark())
        throw std::invalid_argument("messages: invalid page bounds");
    std::lock_guard lock(state_->mutex);
    Statement query(state_->db,history ?
        "SELECT m.id,r.rev,m.utc,m.source,m.submission,m.envelope,r.body FROM messages m JOIN receipts r "
        "ON r.rev=(SELECT max(rev) FROM receipts WHERE message=m.id AND rev<=?2) WHERE m.id<=?1 ORDER BY m.id DESC LIMIT ?3" :
        "SELECT m.id,r.rev,m.utc,m.source,m.submission,m.envelope,r.body FROM receipts r JOIN messages m ON m.id=r.message "
        "WHERE r.rev>?1 AND r.rev<=?2 ORDER BY r.rev LIMIT ?3");
    query.bind(1,history ? (cursor ? std::min(through,cursor-1) : through) : cursor);
    query.bind(2,through);
    query.bind(3,limit+1);
    MessagePage result;
    std::size_t bytes = 0;
    while (query.next()) {
        bytes += sqlite3_column_bytes(query.stmt,5)+sqlite3_column_bytes(query.stmt,6);
        if (result.records.size()==limit || (!result.records.empty() && bytes>2*1024*1024)) {
            result.more = true;
            break;
        }
        result.records.push_back(row(query));
    }
    return result;
}

// 只按重启待办的主键前进，查询不会领走或重置任何消息。
std::vector<StoredMessage> MessageStore::unfinished(std::int64_t after) const {
    std::lock_guard lock(state_->mutex);
    Statement query(state_->db,"SELECT m.id,r.rev,m.utc,m.source,m.submission,m.envelope,r.body FROM outbox o "
        "JOIN messages m ON m.id=o.message JOIN receipts r ON r.rev=m.latest WHERE o.message>? ORDER BY o.message LIMIT 8");
    query.bind(1,after);
    std::vector<StoredMessage> result;
    while (query.next()) result.push_back(row(query));
    return result;
}
}
