#include "message_store_db.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <charconv>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
// 将稳定信号解析成规范十进制序号，跨档案和别名在调用边界拒绝。
std::int64_t letter_id(std::string_view s) {
    std::int64_t id=0;
    const auto [end,error]=std::from_chars(s.data(),s.data()+s.size(),id);
    if (error!=std::errc{} || end!=s.data()+s.size() || id<=0 || s.front()=='0') throw std::invalid_argument("letter: invalid reference");
    return id;
}
// 按实际编码字节截取 UTF-8 原文，offset 必须落在字符边界。
std::string part(const std::string& value,std::size_t offset,std::size_t budget) {
    if (offset>value.size() || (offset<value.size() && (static_cast<unsigned char>(value[offset])&0xc0)==0x80))
        throw std::invalid_argument("letter: invalid offset");
    auto n=std::min(budget-2,value.size()-offset);
    for (;;) {
        while (n && offset+n<value.size() && (static_cast<unsigned char>(value[offset+n])&0xc0)==0x80) --n;
        auto out=value.substr(offset,n);
        if (Json(out).dump().size()<=budget) return out;
        n/=2;
    }
}
constexpr char allowed[]=" AND (source=0 OR EXISTS(SELECT 1 FROM personal_memory p WHERE p.message=child_letter.source AND p.active=1)) ";
}

// 将一次交付的状态、正文及明确导出的原件复制为接收方的不可变信件。
// 步骤：校验模式和原身份 → 查原交付或核实终态 → 固定摘要与可读副本 → 原子入待送箱。
std::int64_t MessageStore::post_letter(std::int64_t child,const std::string& exchange,unsigned mode,const std::string& persona,
    bool start,std::string_view outcome,const std::string& summary,const std::string& body,std::uint64_t note) {
    if (mode<1 || mode>4 || exchange.size()>128 || exchange.find_first_not_of(" ")==exchange.npos ||
        std::ranges::any_of(exchange,[](unsigned char c){return c<32 || c==127;}) || !runtime::valid_utf8(exchange) ||
        summary.size()>480 || body.size()>8192 || !runtime::valid_utf8(summary) || !runtime::valid_utf8(body) ||
        (mode==1 && (!summary.empty() || !body.empty() || note)) || (mode!=3 && note) || note>INT64_MAX)
        throw std::invalid_argument("letter: invalid delivery");
    const auto signature=platform::sha256(Json::array({mode,persona,summary,body,note}).dump());
    const auto phase=start ? "start" : "end";
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    Statement owner(db,"SELECT plugin,preset,subject,channel,source,access FROM child_owner WHERE id=? AND access<>0");
    owner.bind(1,child); if (!owner.next()) throw std::invalid_argument("letter: inactive owner");
    Statement previous(db,"SELECT id,signature FROM child_letter WHERE owner=? AND exchange=? AND phase=?");
    previous.bind(1,child); previous.bind(2,exchange); previous.bind(3,phase);
    if (previous.next()) {
        if (previous.text(1)!=signature) throw std::invalid_argument("letter: conflicting replay");
        const auto id=previous.number(0); tx.commit(); return id;
    }
    if (!start && outcome.empty()) throw std::invalid_argument("letter: no observed exit for exchange");
    const auto source=owner.number(4);
    Json envelope{{"plugin",owner.text(0)},{"preset",owner.number(1)},{"subject",owner.text(2)},{"exchange",exchange},
        {"phase",phase},{"mode",mode==1 ? "status" : mode==2 ? "summary" : mode==3 ? "original" : "digest"},{"audience","owner"},
        {"author",conversation()+"/child/"+std::to_string(child)},{"outcome",outcome}};
    if (!owner.text(3).empty()) envelope["channel"]=owner.text(3);
    if (!persona.empty()) envelope["persona"]=persona;
    if (!start) {
        Statement opened(db,"SELECT id FROM child_letter WHERE owner=? AND exchange=? AND phase='start'");
        opened.bind(1,child); opened.bind(2,exchange);
        if (!opened.next()) throw std::invalid_argument("letter: missing start");
        envelope["startRef"]=conversation()+"/letter/"+std::to_string(opened.number(0));
        if (!summary.empty() || !body.empty()) {
            envelope["summary"]=part(summary.empty() ? body : summary,0,480);
            envelope["summaryKind"]=summary.empty() ? "excerpt" : "sender_summary";
            envelope["partial"]=envelope.at("summary").get_ref<const std::string&>().size()<(summary.empty() ? body.size() : summary.size());
            if (!summary.empty() && envelope.at("partial")==true) envelope["senderSummary"]=summary;
        }
        if (mode>=3 && !body.empty()) envelope["body"]=body;
        if (note) {
            Statement copy(db,"SELECT content FROM child_note WHERE owner=? AND id=? AND (source=0 OR (source=? AND "
                "EXISTS(SELECT 1 FROM personal_memory p WHERE p.active=1 AND p.message=child_note.source)))");
            copy.bind(1,child); copy.bind(2,static_cast<std::int64_t>(note)); copy.bind(3,source);
            if (!(owner.number(5)&1) || !copy.next()) throw std::invalid_argument("letter: unavailable export");
            envelope["note"]=copy.text(0);
            envelope["noteOrigin"]=conversation()+"/child/"+std::to_string(child)+"/note/"+std::to_string(note);
        }
    }
    Statement add(db,"INSERT INTO child_letter(owner,exchange,phase,signature,envelope,source) VALUES(?,?,?,?,?,?)");
    add.bind(1,child); add.bind(2,exchange); add.bind(3,phase); add.bind(4,signature); add.bind(5,envelope.dump()); add.bind(6,source); add.next();
    const auto id=sqlite3_last_insert_rowid(db);
    if (!start && mode==4 && !body.empty()) {
        Statement draft(db,"INSERT INTO letter_digest(letter,state,selection) VALUES(?,'pending','[]')"); draft.bind(1,id); draft.next();
    }
    sql(db,"UPDATE conversation SET memory_revision=1 WHERE memory_revision=0"); tx.commit(); return id;
}
// 读取跨启动仍需经过真实宿主信箱的最早指针。
std::int64_t MessageStore::pending_letter() const {
    std::lock_guard lock(state_->mutex);
    Statement next(state_->db,"SELECT id FROM child_letter WHERE received=0 ORDER BY id LIMIT 1");
    return next.next() ? next.number(0) : 0;
}
// 恢复相同委托时先检查原终态，有结束信的交付仅允许重取原回执。
bool MessageStore::letter_finished(std::int64_t child,const std::string& exchange) const {
    std::lock_guard lock(state_->mutex);
    Statement get(state_->db,"SELECT 1 FROM child_letter WHERE owner=? AND exchange=? AND phase='end'");
    get.bind(1,child); get.bind(2,exchange); return get.next();
}
// 宿主取到队首时登记收件，崩溃重送仅更新同一行。
void MessageStore::receive_letter(std::int64_t id) {
    std::lock_guard lock(state_->mutex);
    Statement mark(state_->db,"UPDATE child_letter SET received=1 WHERE id=? AND received=0");
    mark.bind(1,id); mark.next();
}
// 只向获准的主人私聊提示收件箱有待确认的材料。
bool MessageStore::unread_letter() const {
    std::lock_guard lock(state_->mutex);
    const auto sql=std::string("SELECT 1 FROM child_letter WHERE received=1 AND seen=0")+allowed+" LIMIT 1";
    Statement get(state_->db,sql.c_str()); return get.next();
}

// 从接收方快照读概览或逐字导出的副本，不打开原子库。
Json MessageStore::search_letters(const std::vector<std::string>& topics,unsigned count,std::size_t offset) const {
    if (topics.size()!=1 || topics[0].empty() || topics[0].size()>160 || !count) throw std::invalid_argument("letter: one selector required");
    const auto& term=topics[0]; const auto prefix=conversation()+"/letter/";
    Json result{{"items",Json::array()},{"limited",false}};
    std::int64_t id=0; std::string section;
    if (term.starts_with(prefix)) {
        const auto rest=std::string_view(term).substr(prefix.size()); const auto slash=rest.find('/');
        id=letter_id(rest.substr(0,slash));
        if (slash!=rest.npos) {section=rest.substr(slash+1); if (section!="note" && section!="body" && section!="summary") return result;}
    } else if (term.find("/letter/")!=term.npos) return result;
    if (offset && section.empty()) throw std::invalid_argument("letter: offset requires an exported original");
    std::lock_guard lock(state_->mutex);
    const auto filter=id ? " AND id=?" : term=="待收" ? " AND seen=0" : " AND exchange=?";
    const auto query=std::string("SELECT id,envelope,seen FROM child_letter WHERE received=1")+filter+allowed+
        (term=="待收" ? " ORDER BY id LIMIT 4" : " ORDER BY id DESC LIMIT 4");
    Statement get(state_->db,query.c_str());
    if (id) get.bind(1,id); else if (term!="待收") get.bind(1,term);
    while (get.next()) {
        const auto ref=prefix+std::to_string(get.number(0)); auto item=Json::parse(get.text(1));
        if (section.empty()) {
            for (const auto key : {"body","note"}) if (item.erase(key)) item[std::string(key)+"Ref"]=ref+"/"+key;
            if (item.erase("senderSummary")) item["summaryRef"]=ref+"/summary";
            item["reference"]=ref; item["acknowledged"]=get.number(2)!=0;
            if (item.at("mode")=="digest" && item.at("phase")=="end") {
                Statement edit(state_->db,"SELECT state,selection FROM letter_digest WHERE letter=?"); edit.bind(1,get.number(0));
                if (edit.next()) {
                    item["digestState"]=edit.text(0);
                    if (edit.text(0)=="ready") {
                        item["digestEditor"]="main"; item["digest"]=Json::parse(edit.text(1));
                        if (item.value("summaryKind",std::string{})=="excerpt") {item.erase("summary"); item.erase("summaryKind"); item.erase("partial");}
                    }
                } else item["digestState"]="no_body";
            }
            // 长身份和摘要同时出现时先保留角色、原文位置及各自的原件入口。
            if (item.dump().size()+2>2048 && item.contains("digest")) for(auto& pick:item["digest"]) {
                const auto text=pick.at("text").get<std::string>(); pick["text"]=part(text,0,64);
                if(pick.at("text")!=text) pick["partial"]=true;
            }
            if (item.dump().size()+2>2048 && item.contains("summary")) {
                if(item.at("summaryKind")=="sender_summary") item["summaryRef"]=ref+"/summary";
                item["summary"]=part(item.at("summary").get_ref<const std::string&>(),0,80); item["partial"]=true;
            }
            if (item.dump().size()+2>2048 && item.contains("digest")) {
                for(auto& pick:item["digest"]) {pick["text"]=""; pick["partial"]=true;}
            }
        } else {
            const bool summary=section=="summary";
            if (!item.contains(section) || (summary && item.value("summaryKind",std::string{})!="sender_summary")) continue;
            const auto& raw=item.at(summary && item.contains("senderSummary") ? "senderSummary" : section).get_ref<const std::string&>(); auto chunk=part(raw,offset,640);
            Json original{{"reference",ref+"/"+section},{"letterRef",ref},{"author",item.at("author")},{"audience","owner"},
                {"origin",summary ? "sender_summary" : section=="note" ? "exported_note" : "sender_body"},{"text",chunk}};
            if (section=="note") original["noteOrigin"]=item.at("noteOrigin");
            if (offset+chunk.size()<raw.size()) original["nextOffset"]=offset+chunk.size();
            item=std::move(original);
        }
        auto& items=result.at("items");
        items.push_back(std::move(item));
        if (items.size()>count || items.dump().size()>2048) {items.erase(items.end()-1); result["limited"]=true; break;}
    }
    return result;
}
// 校验当前权限并原子签收已向主 Agent 展示的信号，重试不消耗下一封。
void MessageStore::acknowledge_letters(const std::vector<std::string>& references) {
    if (references.empty() || references.size()>3) throw std::invalid_argument("letter: invalid acknowledgements");
    const auto prefix=conversation()+"/letter/";
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    for (const auto& ref : references) {
        if (!ref.starts_with(prefix)) throw std::invalid_argument("letter: different profile");
        const auto query=std::string("UPDATE child_letter SET seen=1 WHERE received=1 AND id=?")+allowed;
        Statement mark(db,query.c_str()); mark.bind(1,letter_id(std::string_view(ref).substr(prefix.size()))); mark.next();
        if (!sqlite3_changes(db)) throw std::invalid_argument("letter: unavailable");
    }
    tx.commit();
}
}
