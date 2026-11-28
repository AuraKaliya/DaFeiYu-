#include "main_messages.hpp"
#include "message_store_db.hpp"
#include <dafeiyu/model_messages.hpp>
#include <utility>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
constexpr auto summary_kind="recent_dialogue";

// 切取便于接续的 UTF-8 片段，超过上限时标明省略。
std::string excerpt(const std::string& text) {
    if (text.size()<=80) return text;
    auto end=std::size_t{80};
    while ((static_cast<unsigned char>(text[end]) & 0xc0)==0x80) --end;
    return text.substr(0,end)+"…";
}

// 只读取与当前记忆修订一致的检查点，撤权即停止复用其全部派生内容。
Json checkpoint(sqlite3* db) {
    Statement get(db,"SELECT body,tone,through_message FROM recent_dialogue WHERE slot=1 AND authority=(SELECT memory_revision FROM conversation)");
    if (!get.next()) return nullptr;
    return {{"groups",Json::parse(get.text(0))},{"toneReference",get.text(1)},{"through",get.number(2)}};
}
}

// 逐字核对当前宿主给出的原话和实际送达的回答，保留两者各自的持久身份。
// 步骤：校验完整轮次和发布记录 → 摘取原件开头 → 按本次权限修订生成摘要。
std::optional<std::string> MessageStore::summarize_dialogue(std::span<const model::Message> messages) const {
    if (messages.empty() || messages.size()%2) return {};
    std::lock_guard lock(state_->mutex);
    auto* db=state_->db;
    Statement authority(db,"SELECT memory_revision FROM conversation"); authority.next();
    auto items=Json::array();
    for (std::size_t index=0; index<messages.size(); index+=2) {
        const auto& user=messages[index];
        const auto& reply=messages[index+1];
        if (user.source!=model::MessageSource::human || !user.source_message || reply.source!=model::MessageSource::model ||
            reply.completed_dialogue_span!=2)
            return {};
        const auto id=static_cast<std::int64_t>(user.source_message);
        if (owner_quote(db,id)!=user.content) return {};
        Statement source(db,"SELECT m.id FROM messages m JOIN receipts r ON r.rev=m.latest WHERE m.source='main' "
            "AND json_extract(m.envelope,'$.replyTo')=?1 AND json_extract(m.envelope,'$.text')=?2 "
            "AND json_extract(r.body,'$.state')='delivered' AND NOT EXISTS (SELECT 1 FROM personal_memory p "
            "WHERE p.active=0 AND p.message IN (?1,m.id)) ORDER BY m.id DESC LIMIT 1");
        source.bind(1,std::to_string(id)); source.bind(2,reply.content);
        if (!source.next()) return {};
        items.push_back({{"reference",conversation()+"/"+std::to_string(id)},{"user",excerpt(user.content)},
            {"replyReference",conversation()+"/"+std::to_string(source.number(0))},{"assistant",excerpt(reply.content)}});
    }
    return Json{{"source",summary_kind},{"authority",authority.number(0)},{"items",std::move(items)}}.dump();
}

// 单次写入只保存本轮实际采用的全部范围，重试与旧回合不能覆盖较新的检查点。
void MessageStore::save_recent_dialogue(std::int64_t current,const Json& groups,std::string_view tone) {
    std::lock_guard lock(state_->mutex);
    Statement save(state_->db,"INSERT INTO recent_dialogue SELECT 1,?1,memory_revision,?2,?3 FROM conversation WHERE memory_revision=?4 "
        "ON CONFLICT(slot) DO UPDATE SET through_message=excluded.through_message,authority=excluded.authority,body=excluded.body,tone=excluded.tone "
        "WHERE excluded.through_message>=recent_dialogue.through_message AND "
        "(excluded.through_message<>recent_dialogue.through_message OR excluded.authority<>recent_dialogue.authority "
        "OR excluded.body<>recent_dialogue.body OR excluded.tone<>recent_dialogue.tone)");
    save.bind(1,current); save.bind(2,groups.dump()); save.bind(3,tone);
    save.bind(4,groups.front().at("authority").get<std::int64_t>()); save.next();
}

// 从提交过的检查点取最近一组短摘要，完整索引继续供按号核对。
Json MessageStore::restored_dialogue() const {
    std::lock_guard lock(state_->mutex);
    const auto saved=checkpoint(state_->db);
    if (saved.is_null()) return nullptr;
    return {{"summary",saved.at("groups").back().at("items").dump()},{"toneReference",saved.at("toneReference")},
        {"through",saved.at("through")}};
}

// 按摘要中已授权的原号查原件，正文超过预算时保留受限状态。
Json MessageStore::recent_original(std::string_view reference) const {
    const auto id=memory_message(reference,conversation());
    std::lock_guard lock(state_->mutex);
    const auto saved=checkpoint(state_->db);
    if (saved.is_null()) return nullptr;
    for (const auto& group : saved.at("groups")) for (const auto& item : group.at("items")) {
        const bool user=item.at("reference").get_ref<const std::string&>()==reference;
        if (!user && item.at("replyReference").get_ref<const std::string&>()!=reference) continue;
        Statement source(state_->db,"SELECT envelope,utc FROM messages WHERE id=?1 "
            "AND NOT EXISTS(SELECT 1 FROM personal_memory WHERE message=?1 AND active=0)");
        source.bind(1,id);
        if (!source.next()) return nullptr;
        Json results=Json::array({{{"reference",reference},{"topic","近期对话"},{"source",user ? "human" : "model"},
            {"utcMs",source.number(1)},{"quote",Json::parse(source.text(0)).at("text")}}});
        const bool limited=results.dump().size()>2048;
        return {{"items",limited ? Json::array() : std::move(results)},{"limited",limited}};
    }
    return nullptr;
}

// 清空显式会话时让已保存的摘要退出恢复路径。
void MessageStore::clear_recent_dialogue() {
    std::lock_guard lock(state_->mutex); sql(state_->db,"DELETE FROM recent_dialogue");
}

// 仅对实际收到的主人私聊绑定投影，派生写入失败沿用当前可读原件。
bool MainMessages::prepare_recent_context(agent::RunOptions& options) {
    if (!recent_summary_ || !incoming_dialogue_ || incoming_task_ || !reply_tools_) return false;
    options.prompt_message=static_cast<std::uint64_t>(incoming_dialogue_);
    options.summarize_dialogue=[channel=weak_from_this()](std::span<const model::Message> messages) -> std::optional<std::string> {
        if (auto current=channel.lock()) {
            try { return current->store_->summarize_dialogue(messages); }
            catch (const std::runtime_error&) { current->storage_failed(); }
        }
        return {};
    };
    options.save_dialogue_summary=[channel=weak_from_this(),id=incoming_dialogue_](const model::Request& request) {
        const auto current=channel.lock();
        if (!current) return;
        auto groups=Json::array();
        for (const auto& message : request.messages) {
            if (message.source!=model::MessageSource::host_summary || message.content.size()>2048 ||
                message.content.find(R"("source":"recent_dialogue")")==std::string::npos) continue;
            auto summary=Json::parse(message.content);
            if (summary.value("source","")==summary_kind) groups.push_back(std::move(summary));
        }
        if (!groups.empty()) try { current->store_->save_recent_dialogue(id,groups,current->tone_pending_.value_or("")); }
        catch (const std::runtime_error&) { current->storage_failed(); }
    };
    return true;
}

// 冷启动后的首次同受众决策提供短摘要和待查号，正文仍按号取得。
std::optional<agent::DecisionStatusSource> MainMessages::restored_recent_context() {
    if (!std::exchange(restore_recent_due_,false)) return {};
    Json value;
    std::int64_t through=0;
    try {
        value=store_->restored_dialogue();
        if (value.is_null()) return {};
        through=value.at("through").get<std::int64_t>(); value.erase("through");
        const auto& ref=value.at("toneReference").get_ref<const std::string&>();
        const auto tone=ref.empty() ? Json(nullptr) : store_->tone_original(ref);
        if (tone.is_null()) value.erase("toneReference");
        else value["toneState"]=tone.at("state");
    } catch (const std::runtime_error&) { storage_failed(); return {}; }
    const auto* schema=R"([{"key":"context.recent_dialogue","version":1,"description":"最近已结算的主人私聊摘录，按完整原件号查证，当前请求优先。","defaultAttention":{"mode":"explicit"},"snapshotRead":false,"valueSchema":{"type":"object","properties":{"summary":{"type":"string"},"toneReference":{"type":"string"},"toneState":{"type":"string"}},"additionalProperties":false}}])";
    return agent::DecisionStatusSource{"context",schema,Json::array({{{"key","context.recent_dialogue"},
        {"version",1},{"subject",store_->conversation()},{"fact",through},{"value",std::move(value)}}}).dump()};
}

// 明确清空时放弃待恢复的短摘要和派生读口。
void MainMessages::clear_recent_context() {
    restore_recent_due_=false;
    store_->clear_recent_dialogue();
}
}
