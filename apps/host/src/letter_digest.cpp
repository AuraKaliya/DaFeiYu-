#include "message_store_db.hpp"
#include <algorithm>
#include <set>

namespace dfy::app {
using namespace message_db;
using Json=nlohmann::json;
namespace {
struct Span {std::size_t begin,end;};
// 空格与换行只影响显示；出处仍是原接收方正文的 UTF-8 字节位置。
void add_span(const std::string& body,std::vector<Span>& spans,std::size_t begin,std::size_t end) {
    const auto first=body.find_first_not_of(" \t\r\n",begin);
    if (first==body.npos || first>=end) return;
    spans.push_back({first,body.find_last_not_of(" \t\r\n",end-1)+1});
}
// 尽量按原句分段，超长连续文本按字符边界拆分，碎片极多时仍覆盖整封原信。
// 步骤：扫描边界 → 保留原字节范围 → 必要时合并相邻段 → 按原顺序提供片段。
Json passages(const std::string& body) {
    std::vector<Span> spans;
    std::size_t begin=0;
    for(std::size_t pos=0;pos<body.size();) {
        const auto at=pos++;
        while(pos<body.size() && (static_cast<unsigned char>(body[pos])&0xc0)==0x80) ++pos;
        const auto ch=std::string_view(body).substr(at,pos-at);
        const bool punctuation=ch=="。" || ch=="！" || ch=="？" || ch=="!" || ch=="?" || ch=="\n" ||
            (ch=="." && (pos==body.size() || body[pos]==' ' || body[pos]=='\n'));
        if (punctuation || pos-begin>=224 || pos==body.size()) {add_span(body,spans,begin,pos); begin=pos;}
    }
    if(spans.size()>64) {
        std::vector<Span> groups;
        const auto minimum=(body.size()+47)/48;
        for (const auto& s:spans) {
            if (!groups.empty() && groups.back().end-groups.back().begin<minimum) groups.back().end=s.end;
            else groups.push_back(s);
        }
        spans=std::move(groups);
    }
    Json result=Json::array();
    for(const auto& s:spans) result.push_back({{"offset",s.begin},{"text",body.substr(s.begin,s.end-s.begin)}});
    return result;
}
// 取仍授权给接收方的完整原信，原发件人始终来自持久快照。
std::optional<Json> pending(sqlite3* db,const std::string& profile,std::int64_t id) {
    Statement get(db,"SELECT d.letter,l.envelope FROM letter_digest d JOIN child_letter l ON l.id=d.letter "
        "WHERE d.state='pending' AND l.received=1 AND (?1=0 OR d.letter=?1) AND (l.source=0 OR EXISTS("
        "SELECT 1 FROM personal_memory p WHERE p.message=l.source AND p.active=1)) ORDER BY d.letter LIMIT 1");
    get.bind(1,id); if(!get.next()) return {};
    const auto n=get.number(0); const auto env=Json::parse(get.text(1));
    return Json{{"letter",profile+"/letter/"+std::to_string(n)},{"through",n},{"author",env.at("author")},
        {"passages",passages(env.at("body"))}};
}
// 控制一条摘录进前台的编码大小，全文仍可按同一字节位置续读。
std::string snippet(const std::string& input) {
    if (Json(input).dump().size()<=150) return input;
    auto n=std::min<std::size_t>(148,input.size());
    for(;;) {
        while(n && n<input.size() && (static_cast<unsigned char>(input[n])&0xc0)==0x80) --n;
        const auto part=input.substr(0,n);
        if(Json(part).dump().size()<=150) return part;
        n/=2;
    }
}
}
// 待整理的原信在真实收件后才进入私下整理队列。
std::optional<Json> MessageStore::next_digest() const {
    std::lock_guard lock(state_->mutex); return pending(state_->db,conversation(),0);
}
// 解读只能选择已收到的逐字片段，不能创作新的报告事实。
// 步骤：核实当前原件及授权 → 检查所选角色和片段 → 从原文取字 → 原子结算。
void MessageStore::commit_digest(const Json& batch,const Json& proposal) {
    std::lock_guard lock(state_->mutex); auto* db=state_->db; Transaction tx(db);
    const auto id=batch.at("through").get<std::int64_t>(); const auto live=pending(db,conversation(),id);
    if(!live || *live!=batch) throw std::invalid_argument("digest: stale delivery");
    Json selected=Json::array();
    if (proposal!=Json{{"defer",true}}) {
        if (!proposal.is_object() || proposal.size()!=1 || !proposal.contains("picks") || !proposal.at("picks").is_array() ||
            proposal.at("picks").empty() || proposal.at("picks").size()>3) throw std::invalid_argument("digest: invalid selection");
        std::set<std::string> roles; std::set<std::uint64_t> offsets;
        const auto& spans=batch.at("passages");
        for(const auto& p:proposal.at("picks")) {
            if(!p.is_object() || p.size()!=2 || !p.contains("role") || !p.at("role").is_string() || !p.contains("offset") ||
                !p.at("offset").is_number_unsigned()) throw std::invalid_argument("digest: invalid anchor");
            const auto role=p.at("role").get<std::string>(); const auto offset=p.at("offset").get<std::uint64_t>();
            if((role!="activity" && role!="reported_result" && role!="open_question") || !roles.insert(role).second || !offsets.insert(offset).second)
                throw std::invalid_argument("digest: duplicate or unknown category");
            const auto found=std::ranges::find_if(spans,[&](const auto& span){return span.at("offset")==offset;});
            if(found==spans.end()) throw std::invalid_argument("digest: unseen passage");
            const auto& raw=found->at("text").get_ref<const std::string&>(); const auto quote=snippet(raw);
            selected.push_back({{"role",role},{"offset",offset},{"endOffset",offset+raw.size()},{"text",quote},{"partial",quote.size()!=raw.size()}});
        }
    }
    Statement update(db,"UPDATE letter_digest SET state=?,selection=? WHERE letter=?"); update.bind(1,selected.empty() ? "fallback" : "ready");
    update.bind(2,selected.dump()); update.bind(3,id); update.next();
    complete_idle_queue(db,IdleQueue::letter_digest); tx.commit();
}
}
