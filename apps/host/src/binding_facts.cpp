#include "binding_facts.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <set>
#include <tuple>

namespace dfy::app {
namespace {
using Json = nlohmann::json;
// 将字段、作用域、权限和原参数一起绑定到此次提交，同号异参不能获得旧回执。
std::string digest(const plugins::SubagentInput& in) {
    const auto& f = *in.frame;
    auto tools = in.tools;
    std::ranges::sort(tools);
    return platform::sha256(Json::array({f.epoch,f.sequence,f.scope,f.baseline,in.prompt,in.instructions,in.binding,
        tools,in.allow_changes,in.replace_history,in.player_facing,static_cast<int>(in.thinking)}).dump());
}
// 解析有界原帧，拒绝重复键及深层嵌套，字段内容始终作为普通数据。
Json object(std::string_view raw) {
    if (raw.empty() || raw.size() > 64*1024) throw std::invalid_argument("binding: invalid frame size");
    std::vector<std::set<std::string>> keys;
    auto value = Json::parse(raw,[&](int depth, Json::parse_event_t event, Json& part) {
        if (depth > 48) throw std::invalid_argument("binding: fact depth exceeded");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && !keys.back().insert(part.get<std::string>()).second)
            throw std::invalid_argument("binding: duplicate fact key");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    },false);
    if (!value.is_object()) throw std::invalid_argument("binding: expected fact object");
    return value;
}
// 核对数据字段的长度及保留名称，不允许补丁覆盖宿主的来源和工作回执。
void field(std::string_view name) {
    if (name.empty() || name.size() > 80 || name == "factFrame" || name == "hostWorkReceipts")
        throw std::invalid_argument("binding: invalid fact field");
}
// 为省略的原回执正文保留完整字节数和指纹，同项的接纳号定位原记录。
Json body_reference(const std::string& body) {
    return {{"bytes",body.size()},{"sha256",platform::sha256(body)}};
}
}

// 查找已经发布的同号原件。窗口外旧号和同号异参都不能作为新工作派发。
std::uint64_t BindingFacts::prior(const plugins::SubagentInput& input) const {
    const auto& frame = input.frame.value();
    for (const auto& old : admitted_) if (old.epoch == frame.epoch && old.sequence == frame.sequence) {
        if (old.digest != digest(input)) throw std::invalid_argument("binding: conflicting retransmission");
        return old.request;
    }
    if (frame.epoch < epoch_ || (epoch_ && frame.epoch == epoch_ && frame.sequence <= sequence_))
        throw std::invalid_argument("binding: stale frame");
    return 0;
}
// 从同一来源按序构造候选索引及模型输入，准入失败时由调用者丢弃整个副本。
// 步骤：核对来源与连续性 → 合入变更及关闭项 → 形成基线或增量 → 绑定原请求的防重身份。
std::string BindingFacts::apply(const plugins::SubagentInput& input, std::uint64_t request, bool full) {
    const auto& frame = input.frame.value();
    if (!request || !frame.epoch || !frame.sequence || frame.sequence == UINT64_MAX || frame.scope.empty() ||
        frame.scope.size() > 256 || !runtime::valid_utf8(frame.scope) || prior(input))
        throw std::invalid_argument("binding: invalid source or reused frame");
    const bool rebound = frame.epoch != epoch_;
    if (!rebound && frame.scope != scope_) throw std::invalid_argument("binding: mismatched scope");
    const auto expected = rebound ? 1 : sequence_+1;
    if ((!frame.baseline && (rebound || frame.sequence != expected)) || (frame.baseline && frame.sequence < expected))
        throw plugins::SubagentGap(expected);
    const auto patch = object(input.prompt);
    if (patch.size() != 2 || !patch.contains("set") || !patch.at("set").is_object() ||
        !patch.contains("closed") || !patch.at("closed").is_array() || (frame.baseline && !patch.at("closed").empty()))
        throw std::invalid_argument("binding: invalid patch");
    auto next = frame.baseline ? Json::object() : fields_;
    const auto& changes = patch.at("set");
    for (const auto& [name,value] : changes.items()) { field(name); next[name] = value; }
    for (const auto& name : patch.at("closed")) {
        if (!name.is_string()) throw std::invalid_argument("binding: invalid closed field");
        const auto& key = name.get_ref<const std::string&>(); field(key);
        if (changes.contains(key) || !next.erase(key)) throw std::invalid_argument("binding: invalid close");
    }
    if (next.size() > 96 || next.dump().size() > 64*1024) throw std::length_error("binding: snapshot capacity exhausted");
    const bool baseline = frame.baseline || rebound || full;
    const auto samples = next.find("decisionStatusSamples");
    if (samples!=next.end() && (!samples->is_array() || samples->size()>32))
        throw std::invalid_argument("binding: invalid decision status samples");
    auto projected = baseline ? next : changes;
    projected.erase("decisionStatusSamples");
    auto closed = baseline ? Json::array() : patch.at("closed");
    std::erase(closed.get_ref<Json::array_t&>(),Json("decisionStatusSamples"));
    projected["factFrame"] = {{"epoch",frame.epoch},{"sequence",frame.sequence},{"scope",frame.scope},
        {"mode",baseline ? "baseline" : "delta"},{"closed",std::move(closed)}};
    auto prompt = projected.dump();
    admitted_.push_back({frame.epoch,frame.sequence,request,digest(input)});
    if (admitted_.size() > 64) admitted_.pop_front();
    epoch_ = frame.epoch; sequence_ = frame.sequence; scope_ = frame.scope; fields_ = std::move(next);
    return prompt;
}

// 暴露当前完整采样集，正文投影始终使用其余领域字段。
std::string BindingFacts::status_samples() const {
    const auto it = fields_.find("decisionStatusSamples");
    return it==fields_.end() ? "[]" : it->dump();
}

// 同一修订须与留存的阶段、实效声明、原接纳序号及正文匹配。
std::uint64_t BindingWorks::prior(const Work& work, const plugins::SubagentWorkFact& fact, const std::string& content) {
    const auto old = std::ranges::find(work.revisions,fact.revision,&Record::revision);
    if (old == work.revisions.end()) throw std::invalid_argument("binding: revision outside replay window");
    if (old->phase != fact.phase || old->content != content || old->final_effect != fact.final_effect ||
        (fact.revision == 1 && fact.admission_sequence != work.sequence))
        throw std::invalid_argument("binding: conflicting work receipt");
    return old->receipt;
}

// 在保留原号的前提下登记新工作，未决实效和未退出的工作始终占有原槽。
// 步骤：核对有序接纳水位 → 需要时准备最早已结算工作的重传指纹 → 发布新槽和有界归档 → 推进水位。
void BindingWorks::admit(const Key& key, Record first, std::uint64_t sequence) {
    if (key.first == admission_epoch_ && sequence <= admission_sequence_)
        throw std::invalid_argument("binding: admission outside replay window");
    auto victim = records_.end();
    if (records_.size() >= 128) {
        for (auto it=records_.begin();it!=records_.end();++it) {
            const auto& work = it->second;
            if (work.sequence && work.exited && work.final_effect && (victim==records_.end() ||
                work.revisions.back().receipt < victim->second.revisions.back().receipt)) victim = it;
        }
        if (victim==records_.end()) throw std::length_error("binding: work capacity exhausted");
    }
    auto inserted = records_.emplace(key,Work{{std::move(first)},sequence}).first;
    if (victim!=records_.end()) {
        try {
            auto archive = victim->second;
            for (auto& revision : archive.revisions) revision.content = platform::sha256(revision.content);
            archived_.emplace_back(victim->first,std::move(archive));
        } catch (...) { records_.erase(inserted); throw; }
        records_.erase(victim);
        if (archived_.size() > 128) archived_.pop_front();
    }
    if (sequence) { admission_epoch_ = key.first; admission_sequence_ = sequence; }
}

// 原工作每个版本只接纳一次，换代后拒绝旧来源的新工作，允许沿原号补报。
// 步骤：校验身份和正文 → 查原修订及退出 → 核对代次与阶段顺序 → 有界保存并返回原接纳号。
std::uint64_t BindingWorks::settle(std::uint64_t current_epoch, const plugins::SubagentWorkFact& fact) {
    if (!fact.epoch || !fact.revision || fact.work.empty() || fact.work.size() > 128 || fact.content.size() > 8192 ||
        !runtime::valid_utf8(fact.work) || fact.phase < DFY_V2_WORK_ACCEPTED || fact.phase > DFY_V2_WORK_EXIT ||
        (fact.admission_sequence && fact.phase != DFY_V2_WORK_ACCEPTED) || (fact.final_effect && fact.phase != DFY_V2_WORK_EFFECT))
        throw std::invalid_argument("binding: invalid work receipt");
    const auto body = object(fact.content).dump();
    const Key key{fact.epoch,fact.work};
    auto found = records_.find(key);
    if (found != records_.end()) {
        const auto& work = found->second;
        if (fact.revision <= work.revisions.back().revision) return prior(work,fact,body);
        if (fact.revision != work.revisions.back().revision+1 || fact.phase == DFY_V2_WORK_ACCEPTED ||
            (work.exited && fact.phase == DFY_V2_WORK_EXIT) || (work.final_effect && fact.phase == DFY_V2_WORK_EFFECT))
            throw std::invalid_argument("binding: stale work transition");
    } else {
        const auto archived = std::ranges::find_if(archived_,[&](const auto& entry) { return entry.first == key; });
        if (archived != archived_.end()) return prior(archived->second,fact,platform::sha256(body));
        if (fact.epoch != current_epoch || fact.revision != 1 || fact.phase != DFY_V2_WORK_ACCEPTED)
            throw std::invalid_argument("binding: original work required");
    }
    if (next_ == UINT64_MAX) throw std::length_error("binding: work receipt numbers exhausted");
    Record entry{fact.phase,body,next_+1,fact.revision,fact.final_effect};
    if (found == records_.end()) admit(key,std::move(entry),fact.admission_sequence);
    else {
        auto& work = found->second;
        auto& record = work.revisions;
        record.push_back(std::move(entry));
        work.exited |= fact.phase == DFY_V2_WORK_EXIT;
        work.final_effect |= fact.final_effect;
        // 接纳与退出各至多一份。窗口满时只移除已有更新取代的最早实效。
        if (record.size() > 16) record.erase(std::ranges::find(record,DFY_V2_WORK_EFFECT,&Record::phase));
    }
    return ++next_;
}
// 归档项均已最终结算，未归档项逐一核对实效与退出。
bool BindingWorks::finalized() const noexcept {
    return std::ranges::all_of(records_,[](const auto& item) { return item.second.exited && item.second.final_effect; });
}

// 沿数字接纳号查回原工作及修订，读取不更新任何水位。
// 步骤：限定原代次 → 查活动原文或归档指纹 → 以同一身份返回有界结果。
std::optional<std::string> BindingWorks::read(std::uint64_t epoch, std::uint64_t receipt) const {
    const auto lookup = [&](const auto& entries, bool archived) -> std::optional<std::string> {
        for (const auto& [key,work] : entries) if (key.first == epoch) {
            const auto stage = std::ranges::find(work.revisions,receipt,&Record::receipt);
            if (stage == work.revisions.end()) continue;
            Json result{{"epoch",epoch},{"work",key.second},{"revision",stage->revision},{"receipt",receipt},
                {"phase",stage->phase},{"finalEffect",stage->final_effect},{"archived",archived}};
            if (archived) result["sha256"] = stage->content;
            else result["body"] = Json::parse(stage->content);
            return result.dump();
        }
        return {};
    };
    if (auto active = lookup(records_,false)) return active;
    return lookup(archived_,true);
}

// 在本帧的真实字节预算内保留原工作的接纳、最近实效和实际退出。
// 步骤：选择本代次以来的变化 → 组装原号及可见正文 → 按需从已退出和较早的记录收缩正文。
Json BindingWorks::view(std::uint64_t epoch, std::uint64_t after, std::size_t byte_limit) const {
    auto result = Json::array();
    for (const auto& [key,work] : records_) if (key.first == epoch && work.revisions.back().receipt > after) {
        const auto& record = work.revisions;
        const auto& last = record.back();
        Json item{{"work",key.second},{"accepted",record.front().receipt},{"revision",last.revision},{"receipt",last.receipt}};
        if (work.final_effect) item["finalEffect"] = true;
        if (record.front().content.size() <= 256) item["source"] = Json::parse(record.front().content);
        for (const auto& stage : record) {
            if (stage.phase == DFY_V2_WORK_EXIT) item["exited"] = stage.receipt;
            if (stage.phase == DFY_V2_WORK_EFFECT) {
                item["effectReceipt"] = stage.receipt;
                if (stage.content.size() <= 1024) item["effect"] = Json::parse(stage.content);
                else item["effect"] = body_reference(stage.content);
            }
        }
        result.push_back(std::move(item));
    }
    auto size = result.dump().size();
    if (size <= byte_limit) return result;

    std::vector<Json*> oldest;
    for (auto& item : result) oldest.push_back(&item);
    std::ranges::sort(oldest,{},[](const Json* item) {
        const bool exited = item->contains("exited");
        return std::tuple{!(exited && item->value("finalEffect",false)),!exited,item->at("receipt").get<std::uint64_t>()};
    });
    for (auto* item : oldest) {
        const auto before = item->dump().size();
        if (item->contains("effect")) {
            const auto body = item->at("effect").dump();
            if (body.size() > 128) (*item)["effect"] = body_reference(body);
        }
        item->erase("source");
        // 极端转义的外部工作名仍可凭接纳号定位，避免名字本身挤掉其他未决工作的身份。
        if (item->at("work").dump().size() > 128) {
            (*item)["workSha256"] = platform::sha256(item->at("work").get_ref<const std::string&>());
            item->erase("work");
        }
        size -= before-item->dump().size();
        if (size <= byte_limit) return result;
    }
    throw std::length_error("binding: work references exceed frame budget");
}
}
