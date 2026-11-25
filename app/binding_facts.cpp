#include "binding_facts.hpp"
#include <dafeiyu/agent_abi_v2.h>
#include <dafeiyu/digest.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <set>

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
    auto projected = baseline ? next : changes;
    projected["factFrame"] = {{"epoch",frame.epoch},{"sequence",frame.sequence},{"scope",frame.scope},
        {"mode",baseline ? "baseline" : "delta"},{"closed",baseline ? Json::array() : patch.at("closed")}};
    auto prompt = projected.dump();
    admitted_.push_back({frame.epoch,frame.sequence,request,digest(input)});
    if (admitted_.size() > 64) admitted_.pop_front();
    epoch_ = frame.epoch; sequence_ = frame.sequence; scope_ = frame.scope; fields_ = std::move(next);
    return prompt;
}

// 原工作每个版本只接纳一次，换代后拒绝旧来源的新工作，允许沿原号补报。
// 步骤：校验身份和正文 → 查原版本及终态 → 核对代次和阶段顺序 → 保存并返回原接纳号。
std::uint64_t BindingWorks::settle(std::uint64_t current_epoch, const plugins::SubagentWorkFact& fact) {
    if (!fact.epoch || !fact.revision || fact.work.empty() || fact.work.size() > 128 || fact.content.size() > 8192 ||
        !runtime::valid_utf8(fact.work) || fact.phase < DFY_V2_WORK_ACCEPTED || fact.phase > DFY_V2_WORK_EXIT)
        throw std::invalid_argument("binding: invalid work receipt");
    const auto body = object(fact.content).dump();
    const Key key{fact.epoch,fact.work};
    auto found = records_.find(key);
    if (found != records_.end()) {
        const auto& record = found->second;
        if (fact.revision <= record.size()) {
            const auto& old = record[static_cast<std::size_t>(fact.revision-1)];
            if (old.phase != fact.phase || old.content != body) throw std::invalid_argument("binding: conflicting work receipt");
            return old.receipt;
        }
        const bool exited = std::ranges::any_of(record,[](const Record& old) { return old.phase == DFY_V2_WORK_EXIT; });
        if (fact.revision != record.size()+1 || fact.phase == DFY_V2_WORK_ACCEPTED || (exited && fact.phase != DFY_V2_WORK_EFFECT))
            throw std::invalid_argument("binding: stale work transition");
        if (record.size() >= 16) throw std::length_error("binding: work revisions exhausted");
    } else if (fact.epoch != current_epoch || fact.revision != 1 || fact.phase != DFY_V2_WORK_ACCEPTED) {
        throw std::invalid_argument("binding: original work required");
    }
    if (next_ == UINT64_MAX || (found == records_.end() && records_.size() >= 128)) throw std::length_error("binding: work capacity exhausted");
    Record entry{fact.phase,body,next_+1};
    if (found == records_.end()) records_.emplace(key,std::vector<Record>{std::move(entry)});
    else found->second.push_back(std::move(entry));
    return ++next_;
}
// 返回原工作的接纳、最近实效和实际退出，跨代迟到结算不进入当前模型范围。
// 步骤：限定当前代次 → 分别归集实效与退出 → 长正文改用原接纳号和指纹引用。
Json BindingWorks::view(std::uint64_t epoch, std::uint64_t after) const {
    auto result = Json::array();
    for (const auto& [key,record] : records_) if (key.first == epoch && record.back().receipt > after) {
        const auto& last = record.back();
        Json item{{"work",key.second},{"accepted",record.front().receipt},{"revision",record.size()},{"receipt",last.receipt}};
        for (const auto& stage : record) {
            if (stage.phase == DFY_V2_WORK_EXIT) item["exited"] = stage.receipt;
            if (stage.phase == DFY_V2_WORK_EFFECT) {
                item["effectReceipt"] = stage.receipt;
                if (stage.content.size() <= 1024) item["effect"] = Json::parse(stage.content);
                else item["effect"] = {{"bytes",stage.content.size()},{"sha256",platform::sha256(stage.content)}};
            }
        }
        result.push_back(std::move(item));
    }
    return result;
}
}
