#include "message_store_db.hpp"
#include <dafeiyu/plugin_agents.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
// 身份元组逐字段绑定；拒绝空白、控制字符和过长标识，不拼接可歧义的路径。
void identifier(std::string_view value) {
    if (value.size()>128 || value.find_first_not_of(" ")==value.npos || !runtime::valid_utf8(value) ||
        std::ranges::any_of(value,[](unsigned char c){return c<32 || c==127;})) throw std::invalid_argument("child memory: invalid identifier");
}
// 校验预设所需的身份维度，插件名来自实际实例或主人的授权命令。
void validate(std::string_view plugin,const plugins::MemoryIdentity& who) {
    identifier(plugin); identifier(who.subject);
    if (who.preset==plugins::MemoryPreset::avatar) identifier(who.channel);
    else if ((who.preset!=plugins::MemoryPreset::task && who.preset!=plugins::MemoryPreset::business) || !who.channel.empty())
        throw std::invalid_argument("child memory: invalid preset or channel");
}
// 绑定同一组数据库列以复用原档案中的持久身份。
void bind_identity(Statement& q,std::string_view plugin,const plugins::MemoryIdentity& who) {
    q.bind(1,plugin); q.bind(2,static_cast<unsigned>(who.preset)); q.bind(3,who.subject); q.bind(4,who.channel);
}
// 为长共享原话取 UTF-8 边界内的有界预览，在投影中另注省略。
std::string preview(const std::string& content) {
    auto n=std::min<std::size_t>(content.size(),768);
    for (;;) {
        while (n<content.size() && (static_cast<unsigned char>(content[n])&0xc0)==0x80) --n;
        auto part=content.substr(0,n);
        if (Json(part).dump().size()<=960) return part;
        n/=2;
    }
}
}

// 任务和独立业务按本插件的委托或对象续接；分身须有该渠道和账号的主人授权。
ChildBinding MessageStore::bind_child(std::string_view plugin,const plugins::MemoryIdentity& who) {
    validate(plugin,who);
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    if (who.preset!=plugins::MemoryPreset::avatar) {
        Statement add(db,"INSERT OR IGNORE INTO child_owner(plugin,preset,subject,channel,access) VALUES(?,?,?,?,3)");
        bind_identity(add,plugin,who); add.next();
    }
    Statement get(db,"SELECT id,source FROM child_owner WHERE plugin=? AND preset=? AND subject=? AND channel=? AND access<>0");
    bind_identity(get,plugin,who);
    if (!get.next()) throw std::invalid_argument("child memory: no active binding");
    const auto id=get.number(0), source=get.number(1);
    tx.commit(); return {id,source ? conversation()+"/"+std::to_string(source) : ""};
}

// 主人逐账号设置读、写和向该渠道转述的权利；共享原话必须是本档案仍可读取的确切原件。
std::int64_t MessageStore::grant_avatar(std::string_view plugin,const plugins::MemoryIdentity& who,int access,std::string_view reference) {
    validate(plugin,who);
    if (who.preset!=plugins::MemoryPreset::avatar || access<0 || access>7 ||
        (((access&4) || !reference.empty()) && !(access&1))) throw std::invalid_argument("child memory: invalid grant");
    std::int64_t source=0;
    if (!reference.empty()) {
        const auto original=recall_memory(reference);
        if (!original || reference!=conversation()+"/"+std::to_string(original->id)) throw std::invalid_argument("child memory: unavailable original");
        source=original->id;
    }
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    if (source) {
        Statement eligible(db,"SELECT 1 FROM personal_memory WHERE message=? AND active=1");
        eligible.bind(1,source); if (!eligible.next()) throw std::invalid_argument("child memory: revoked source");
    }
    Statement add(db,"INSERT INTO child_owner(plugin,preset,subject,channel,access,source) VALUES(?,?,?,?,?,?) "
        "ON CONFLICT(plugin,preset,subject,channel) DO UPDATE SET access=excluded.access,source=excluded.source "
        "WHERE access<>excluded.access OR source<>excluded.source");
    bind_identity(add,plugin,who); add.bind(5,access); add.bind(6,source); add.next();
    if (!sqlite3_changes(db)) { tx.commit(); return 0; }
    Statement get(db,"SELECT id FROM child_owner WHERE plugin=? AND preset=? AND subject=? AND channel=?");
    bind_identity(get,plugin,who); get.next(); const auto id=get.number(0); tx.commit(); return id;
}

// 同一子库的事件只计一次，分身记录带上写入时已授权的原件，撤回后该派生记录也停止投影。
std::int64_t MessageStore::child_note(std::int64_t child,const std::string& event,const std::string& topic,const std::string& content) {
    identifier(event); identifier(topic);
    if (content.size()>768 || content.find_first_not_of(" \t\r\n")==content.npos || !runtime::valid_utf8(content) ||
        Json(content).dump().size()>1024) throw std::invalid_argument("child memory: invalid note");
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement grant(db,"SELECT source FROM child_owner WHERE id=? AND access&2 AND (source=0 OR source IN(SELECT message FROM personal_memory WHERE active=1))");
    grant.bind(1,child); if (!grant.next()) throw std::invalid_argument("child memory: write denied");
    Statement add(db,"INSERT OR IGNORE INTO child_note(owner,event,topic,content,source) VALUES(?,?,?,?,?)");
    add.bind(1,child); add.bind(2,event); add.bind(3,topic); add.bind(4,content); add.bind(5,grant.number(0)); add.next();
    Statement get(db,"SELECT id,topic,content,source FROM child_note WHERE owner=? AND event=?");
    get.bind(1,child); get.bind(2,event); get.next();
    if (get.text(1)!=topic || get.text(2)!=content || get.number(3)!=grant.number(0)) throw std::invalid_argument("child memory: conflicting event");
    const auto id=get.number(0); tx.commit(); return id;
}

// 按持久身份、当轮主题及受众查至多两份自己的记录，另可交付一份主人明确选择的原话。
Json MessageStore::child_projection(std::int64_t child,std::string_view topic,bool outward) const {
    Json items=Json::array();
    if (topic.empty()) return items;
    identifier(topic); std::lock_guard lock(state_->mutex); auto* db=state_->db;
    Statement grant(db,"SELECT source FROM child_owner WHERE id=? AND (access&?2)=?2");
    grant.bind(1,child); grant.bind(2,outward ? 5 : 1);
    if (!grant.next()) return items;
    const auto shared=grant.number(0);
    const auto prefix=conversation()+"/child/"+std::to_string(child)+"/note/";
    Statement source(db,"SELECT p.topic,m.envelope FROM personal_memory p JOIN messages m ON m.id=p.message WHERE p.active=1 AND p.message=?");
    source.bind(1,shared);
    const bool available=source.next();
    if (available && source.text(0)==topic) {
        const auto raw=Json::parse(source.text(1)).at("text").get<std::string>(), part=preview(raw);
        items.push_back({{"origin","main.original"},{"reference",conversation()+"/"+std::to_string(shared)},{"text",part},{"partial",part.size()<raw.size()}});
    }
    Statement notes(db,"SELECT id,content,source FROM child_note WHERE owner=? AND topic=? AND (source=0 OR source=?) ORDER BY id DESC LIMIT 2");
    notes.bind(1,child); notes.bind(2,topic); notes.bind(3,available ? shared : 0);
    while (notes.next()) {
        Json note{{"origin","child.note"},{"reference",prefix+std::to_string(notes.number(0))},{"text",notes.text(1)}};
        if (notes.number(2)) note["sourceRef"]=conversation()+"/"+std::to_string(notes.number(2));
        items.push_back(std::move(note));
        if (items.dump().size()>2048) items.erase(items.end()-1);
    }
    return items;
}
}
