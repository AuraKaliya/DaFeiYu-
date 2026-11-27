#include "message_store.hpp"
#include <winsqlite/winsqlite3.h>
#include <dafeiyu/utf8.hpp>
#include <charconv>
#include <chrono>
#include <mutex>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
// 将系统返回码映射为可公开核对的失败，避免泄漏 SQL 参数或消息正文。
void check(int code) {
    if (code!=SQLITE_OK) throw std::runtime_error("messages: SQLite error "+std::to_string(code));
}
// 执行宿主内置的固定 SQL。
void sql(sqlite3* db, const char* text) { check(sqlite3_exec(db,text,nullptr,nullptr,nullptr)); }

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
}

struct MessageStore::State {
    sqlite3* db = nullptr;
    std::mutex mutex;
    std::string conversation;
    // 关闭此实例打开的唯一数据库句柄。
    ~State() { if (db) sqlite3_close_v2(db); }
};

// 默认回滚日志和完整同步使原文接纳先于业务交付，逻辑会话号只在新库签发。
// 步骤：打开目录内的数据库 → 识别版本 → 原子建表与签发身份 → 读取固定身份。
MessageStore::MessageStore(const std::filesystem::path& profile) : state_(std::make_unique<State>()) {
    std::filesystem::create_directories(profile);
    const auto path = (profile/"main-conversation.sqlite3").u8string();
    check(sqlite3_open_v2(reinterpret_cast<const char*>(path.c_str()),&state_->db,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,nullptr));
    auto* db = state_->db;
    check(sqlite3_busy_timeout(db,150));
    sql(db,"PRAGMA synchronous=FULL");
    Transaction transaction(db);
    {
        Statement version(db,"PRAGMA user_version");
        if (!version.next() || version.number(0)>3) throw std::runtime_error("messages: unsupported database version");
        const auto previous = version.number(0);
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
    }
    Statement identity(db,"SELECT id FROM conversation");
    if (!identity.next()) throw std::runtime_error("messages: missing conversation identity");
    state_->conversation = identity.text(0);
    transaction.commit();
}
// 归还数据库及其串行化状态。
MessageStore::~MessageStore() = default;
// 返回数据库给当前目录签发的逻辑身份。
const std::string& MessageStore::conversation() const noexcept { return state_->conversation; }

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
// 在当前档案内校验原件身份，拒绝跨档案或不完整的消息号。
std::int64_t memory_message(std::string_view identity, const std::string& conversation) {
    if (!identity.starts_with(conversation+"/")) throw std::invalid_argument("memory: different conversation");
    identity.remove_prefix(conversation.size()+1);
    std::int64_t id = 0;
    const auto result = std::from_chars(identity.data(),identity.data()+identity.size(),id);
    if (result.ec!=std::errc{} || result.ptr!=identity.data()+identity.size() || id<=0)
        throw std::invalid_argument("memory: invalid message identity");
    return id;
}
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
std::pair<std::int64_t,bool> MessageStore::change_memory(std::string_view identity, const std::optional<std::string>& topic,
    std::string_view excerpt) {
    const auto id = memory_message(identity,conversation());
    if (topic) check_memory_topic(*topic);
    std::lock_guard lock(state_->mutex);
    auto* db = state_->db;
    Transaction transaction(db);
    Statement prior(db,"SELECT topic,active FROM personal_memory WHERE message=?");
    prior.bind(1,id);
    const bool exists = prior.next();
    bool changed = false;
    if (topic && (!exists || !excerpt.empty())) {
        const auto query = std::string(memory_source)+" AND m.id=?";
        Statement source(db,query.c_str());
        source.bind(1,id);
        if (!source.next() || !eligible_original(source,true)) throw std::invalid_argument("memory: eligible user quote within 2048 bytes required");
        if (!excerpt.empty() && Json::parse(source.text(1)).at("text").get<std::string>().find(excerpt)==std::string::npos)
            throw std::invalid_argument("memory: excerpt differs from original");
    }
    if (topic && !exists) {
        Statement add(db,"INSERT INTO personal_memory VALUES(?,?,1)");
        add.bind(1,id); add.bind(2,*topic); add.next();
        changed = true;
    } else if (!exists || (topic && (prior.text(0)!=*topic || !prior.number(1)))) {
        throw std::invalid_argument("memory: unknown, revoked or conflicting original");
    } else if (!topic && prior.number(1)) {
        Statement revoke(db,"UPDATE personal_memory SET active=0 WHERE message=?");
        revoke.bind(1,id); revoke.next();
        changed = true;
    }
    if (changed) sql(db,topic ? "UPDATE conversation SET memory_revision=1 WHERE memory_revision=0" :
        "UPDATE conversation SET memory_revision=memory_revision+1");
    Statement revision(db,"SELECT memory_revision FROM conversation");
    revision.next();
    const auto result = revision.number(0);
    transaction.commit();
    return {result,changed};
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
    Statement result(state_->db,(select+(by_id ? "p.message=?" : "p.topic=? ORDER BY p.message DESC LIMIT 1")).c_str());
    if (by_id) result.bind(1,id); else result.bind(1,selector);
    return result.next() ? std::optional(row(result)) : std::nullopt;
}
// 按完整主题、局部主题及新近次序取唯一原件，预算不足时继续尝试较短候选。
// 步骤：验证有限主题 → 在授权索引中合并排名 → 按序核算带来源的证据数组。
nlohmann::json MessageStore::search_memory(const std::vector<std::string>& topics, unsigned count) const {
    if (topics.empty() || topics.size()>3 || !count) throw std::invalid_argument("memory: invalid search");
    for (const auto& topic : topics) check_memory_topic(topic);
    std::lock_guard lock(state_->mutex);
    Statement matches(state_->db,"WITH terms(term) AS (VALUES(?1),(?2),(?3)), ranked AS ("
        "SELECT p.message,p.topic,min(CASE WHEN p.topic=t.term THEN 0 ELSE 1 END) AS rank "
        "FROM personal_memory p JOIN terms t ON instr(p.topic,t.term)>0 WHERE p.active=1 "
        "GROUP BY p.message ORDER BY rank,p.message DESC LIMIT 13) "
        "SELECT m.id,r.topic,m.source,m.utc,m.envelope FROM ranked r JOIN messages m ON m.id=r.message "
        "ORDER BY r.rank,m.id DESC");
    for (std::size_t i=0;i<topics.size();++i) matches.bind(static_cast<int>(i+1),topics[i]);
    Json items = Json::array();
    std::size_t bytes = 2;
    bool limited = false;
    unsigned candidates = 0;
    while (matches.next()) {
        if (++candidates>12 || items.size()>=count || items.size()>=3) { limited = true; break; }
        const auto envelope = Json::parse(matches.text(4));
        Json item{{"reference",conversation()+"/"+std::to_string(matches.number(0))},{"topic",matches.text(1)},
            {"source",matches.text(2)},{"utcMs",envelope.value("sourceUtcMs",matches.number(3))},{"quote",envelope.at("text")}};
        const auto cost = item.dump().size() + (items.empty() ? 0 : 1);
        if (bytes+cost>2048) { limited = true; continue; }
        bytes += cost;
        items.push_back(std::move(item));
    }
    return {{"items",std::move(items)},{"limited",limited}};
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
    const auto revision = append_receipt(db,id,receipt);
    if ((source=="desktop" || source=="terminal") && envelope.is_object() && envelope.contains("text") &&
        envelope.at("text").is_string() && envelope.value("sender",Json{})=="user" && envelope.value("route",Json{})=="send" &&
        receipt.value("state",Json{})=="pending") {
        const auto& text = envelope.at("text");
        if (const auto topic = memory_capture_topic(text.get_ref<const std::string&>())) {
            check_memory_topic(*topic);
            if (text.dump().size()>2048) throw std::invalid_argument("memory: quote exceeds 2048 bytes");
            Statement retain(db,"INSERT INTO personal_memory VALUES(?,?,1)");
            retain.bind(1,id); retain.bind(2,*topic); retain.next();
            sql(db,"UPDATE conversation SET memory_revision=1 WHERE memory_revision=0");
        }
    }
    transaction.commit();
    return {{id,revision,utc,std::string(source),std::string(submission),envelope,receipt},true};
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
