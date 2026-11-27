#include "message_store_db.hpp"

namespace dfy::app {
using Json = nlohmann::json;
using namespace message_db;
namespace {
constexpr std::size_t evidence_bytes = 2048;

// 从有边界的完整登记名定位项目，多个同名或同时指向多个登记对象时保持未知。
std::string project_of(sqlite3* db, std::string_view prompt) {
    for (const std::string_view prefix : {"请汇报一下","请汇报","汇报一下","跟我说说","说说","请"}) {
        if (prompt.starts_with(prefix)) { prompt.remove_prefix(prefix.size()); break; }
    }
    Statement names(db,R"(
        WITH input(text) AS (VALUES(dfy_case_fold(?))),
             terms(name,word) AS (SELECT name,dfy_case_fold(term) FROM case_term WHERE kind IN ('name','object','alias'))
        SELECT DISTINCT name FROM terms,input
        WHERE word<>'' AND (instr(text,'「'||word||'」')>0 OR instr(text,'“'||word||'”')>0 OR text=word
          OR substr(text,1,length(word)+1) IN (word||'的',word||' ',word||'，',word||',')
          OR substr(text,1,length(word)+2) IN (word||'进度',word||'进展'))
        LIMIT 2
    )");
    names.bind(1,prompt);
    if (!names.next()) return {};
    auto project = names.text(0);
    return names.next() ? std::string{} : project;
}

struct TargetReply { std::int64_t reply=0,parent=0; std::string question; };
// 只认实际送给主人的近期答复，补证时沿已登记的答复号核验。
// 步骤：固定可见区间 → 核对逐字摘录 → 核对该答复的原收件。
TargetReply target_reply(sqlite3* db,std::int64_t before,std::int64_t exact,std::string_view quote) {
    Statement replies(db,R"(
        SELECT m.id,json_extract(m.envelope,'$.text'),p.id
        FROM messages m JOIN receipts r ON r.rev=m.latest
        JOIN messages p ON p.id=CAST(json_extract(m.envelope,'$.replyTo') AS INTEGER)
        WHERE m.id<?1 AND (?2=0 OR m.id=?2) AND m.source='main' AND p.source IN ('desktop','terminal')
          AND json_extract(r.body,'$.state')='delivered' AND json_extract(m.envelope,'$.sender')='assistant'
        ORDER BY m.id DESC LIMIT 12
    )");
    replies.bind(1,before); replies.bind(2,exact);
    TargetReply target;
    while (replies.next()) {
        if (replies.text(1).find(quote)==std::string::npos) continue;
        if (target.reply) throw std::invalid_argument("tone: ambiguous reply");
        if (auto question=owner_quote(db,replies.number(2)))
            target={replies.number(0),replies.number(2),std::move(*question)};
    }
    if (!target.reply) throw std::invalid_argument("tone: reply not found");
    return target;
}

// 限制模型提供的短提示与出处片段，空白字符串不构成证据。
bool short_text(std::string_view text,std::size_t limit) {
    return text.size()<=limit && text.find_first_not_of(" \t\r\n")!=text.npos && Json(text).dump().size()<=limit;
}

enum class Progress { other, regular, accepted, unknown };
// 首包只识别有明确目的的进度问法，涉及里程碑时根据本轮原话判断验收状态。
Progress progress_of(std::string_view prompt) {
    const auto has=[&](std::string_view cue){return prompt.find(cue)!=prompt.npos;};
    if (has("不聊进度") || has("别报进度") || has("不用汇报")) return Progress::other;
    const bool request=has("说说进度") || has("说说进展") || has("汇报进度") || has("汇报进展") ||
        has("现在到哪") || has("走到哪一步") || has("进展如何") || has("进度如何") ||
        has("进度怎么样") || has("进展怎么样");
    const bool direct=(prompt.starts_with("请汇报") || prompt.starts_with("汇报一下") || prompt.starts_with("跟我说说")) &&
        (has("进度") || has("进展"));
    if (!request && !direct) return Progress::other;
    if (prompt.find("里程碑")==prompt.npos && prompt.find("验收")==prompt.npos) return Progress::regular;
    if (prompt.find("未验收")!=prompt.npos || prompt.find("尚未")!=prompt.npos || prompt.find("还没")!=prompt.npos)
        return Progress::regular;
    if (!has("如果") && !has("是否") && !has("没说") && !has("据说") && !has("听说") &&
        (has("已正式验收") || has("已通过正式验收")))
        return Progress::accepted;
    return Progress::unknown;
}
}

// 保留一次已核实的反馈；只有根据原件成立的范围才进入待发布状态。
// 步骤：核对反馈与所指答复 → 绑定登记项目和补证 → 同号保存或补足原记录。
Json MessageStore::note_tone(std::int64_t message,const Json& proposal) {
    const auto& tone = proposal.at("tone");
    const auto excerpt = proposal.at("excerpt").get<std::string>();
    const auto reply_quote = tone.at("reply").get<std::string>();
    if (!short_text(excerpt,256) || !short_text(reply_quote,512)) throw std::invalid_argument("tone: invalid excerpt");
    const auto scope = proposal.at("scope");
    if (scope=="private" || scope=="one_time") return {{"state","skipped"}};

    std::lock_guard lock(state_->mutex);
    auto* db=state_->db; Transaction tx(db);
    const auto original=owner_quote(db,message);
    if (!original || original->find(excerpt)==original->npos || Json(*original).dump().size()>1024)
        throw std::invalid_argument("tone: invalid feedback");
    if (original->find("只改这句")!=original->npos || original->find("只限这次")!=original->npos) return {{"state","skipped"}};

    const auto previous=tone.contains("prior") ? memory_message(tone.at("prior").get<std::string>(),conversation()) : message;
    const auto reference=conversation()+"/"+std::to_string(previous);
    Statement existing(db,"SELECT reply,project,state,basis,active FROM tone_correction WHERE message=?");
    existing.bind(1,previous);
    std::int64_t reply_id=0;
    std::string previous_project;
    if (existing.next()) {
        const auto status=existing.text(2);
        if (!existing.number(4)) throw std::invalid_argument("tone: revoked");
        if (existing.number(3)==message || previous==message) return {{"state",status},{"reference",reference}};
        if (previous>=message || status!="unclear" || scope!="ongoing") throw std::invalid_argument("tone: invalid clarification");
        reply_id=existing.number(0); previous_project=existing.text(1);
    } else if (previous!=message) throw std::invalid_argument("tone: unknown clarification");

    const auto target=target_reply(db,previous,reply_id,reply_quote);
    auto project=project_of(db,target.question);
    const auto stated=project_of(db,*original);
    if (project.empty()) project=stated;
    if ((!stated.empty() && project!=stated) || (!previous_project.empty() && project!=previous_project))
        throw std::invalid_argument("tone: different project");
    Statement withdrawn(db,"SELECT 1 FROM personal_memory WHERE active=0 AND message IN (?1,?2,?3,?4)");
    withdrawn.bind(1,previous); withdrawn.bind(2,message); withdrawn.bind(3,target.parent); withdrawn.bind(4,target.reply);
    if (withdrawn.next()) throw std::invalid_argument("tone: revoked basis");

    const bool ready=scope=="ongoing" && !project.empty() && progress_of(target.question)!=Progress::other;
    const auto hint=tone.value("hint",std::string{}),reason=tone.value("reason",std::string{});
    const auto condition=tone.value("condition",std::string{"progress"});
    if (ready && (!short_text(hint,256) || !short_text(reason,384))) throw std::invalid_argument("tone: missing basis");
    if (ready && condition=="accepted" && original->find("验收")==original->npos && target.question.find("验收")==target.question.npos)
        throw std::invalid_argument("tone: missing acceptance context");
    if (previous!=message && !ready) throw std::invalid_argument("tone: unclear clarification");
    const auto status=ready ? "draft" : "unclear";
    Json body{{"excerpt",excerpt},{"hint",ready ? hint : ""},{"reason",ready ? reason : ""},{"condition",condition}};
    Statement save(db,R"(
        INSERT INTO tone_correction(message,reply,parent,basis,project,state,body) VALUES(?,?,?,?,?,?,?)
        ON CONFLICT(message) DO UPDATE SET basis=excluded.basis,project=excluded.project,state=excluded.state,body=excluded.body
    )");
    save.bind(1,previous); save.bind(2,target.reply); save.bind(3,target.parent); save.bind(4,message);
    save.bind(5,project); save.bind(6,status); save.bind(7,body.dump()); save.next();
    sql(db,"UPDATE conversation SET memory_revision=1 WHERE memory_revision=0");
    tx.commit();
    return {{"state",status},{"reference",reference}};
}

// 在话题推进或闲时发布准备好的记录，确认存在时挂上实际送达的改写原件。
void MessageStore::settle_tones() {
    std::lock_guard lock(state_->mutex);
    sql(state_->db,R"(
        UPDATE tone_correction SET state='ready',rewrite=coalesce((
            SELECT m.id FROM messages m JOIN receipts r ON r.rev=m.latest
            WHERE m.source='main' AND json_extract(m.envelope,'$.replyTo')=CAST(tone_correction.message AS TEXT)
              AND json_extract(r.body,'$.state')='delivered'
            ORDER BY m.id DESC LIMIT 1),0)
        WHERE state='draft' AND active=1
    )");
}

// 在同一登记对象下从新到旧核对适用条件，只给一条明确适用的短提示。
Json MessageStore::tone_for(std::string_view prompt) const {
    const auto progress=progress_of(prompt);
    if (progress==Progress::other) return nullptr;
    std::lock_guard lock(state_->mutex);
    const auto project=project_of(state_->db,prompt);
    if (project.empty()) return nullptr;
    Statement notes(state_->db,"SELECT message,body FROM tone_correction WHERE project=? AND active=1 AND state='ready' ORDER BY message DESC");
    notes.bind(1,project);
    while (notes.next()) {
        const auto body=Json::parse(notes.text(1));
        if (body.at("condition")=="accepted") {
            if (progress==Progress::unknown) return nullptr;
            if (progress!=Progress::accepted) continue;
        }
        const auto id=notes.number(0);
        return {{"message",id},{"reference",conversation()+"/"+std::to_string(id)},{"project",project},
            {"condition",body.at("condition")},{"hint",body.at("hint")}};
    }
    return nullptr;
}

// 按明确的项目引用选择一条仍获准的未定经历，供模型补证。
std::string MessageStore::unclear_tone(std::string_view project) const {
    std::lock_guard lock(state_->mutex);
    const auto name=project_of(state_->db,project);
    if (name.empty()) return {};
    Statement note(state_->db,"SELECT message FROM tone_correction WHERE project=? AND state='unclear' AND active=1 ORDER BY message DESC LIMIT 1");
    note.bind(1,name);
    return note.next() ? conversation()+"/"+std::to_string(note.number(0)) : std::string{};
}

// 按当前授权展开纠正原文；运行中的待整理视图只携带一段短原话。
Json MessageStore::tone_original(std::string_view reference,bool pending) const {
    const auto id=memory_message(reference,conversation());
    std::lock_guard lock(state_->mutex);
    Statement note(state_->db,"SELECT state,body,reply,basis,rewrite FROM tone_correction WHERE message=? AND active=1");
    note.bind(1,id);
    if (!note.next()) return nullptr;
    const auto status=note.text(0);
    if (pending && status=="ready") return nullptr;
    const auto body=Json::parse(note.text(1));
    Json value{{"message",id},{"reference",reference},{"state",status}};
    if (pending) value["excerpt"]=body.at("excerpt");
    else if (auto quote=owner_quote(state_->db,id)) {
        value.update({{"quote",std::move(*quote)},{"reason",status=="ready" ? body.at("reason") : Json("")},
            {"replyReference",conversation()+"/"+std::to_string(note.number(2))},
            {"basisReference",conversation()+"/"+std::to_string(note.number(3))}});
        if (note.number(4)) value["rewriteReference"]=conversation()+"/"+std::to_string(note.number(4));
    } else return nullptr;
    return Json::array({value}).dump().size()<=evidence_bytes ? value : Json(nullptr);
}
}
