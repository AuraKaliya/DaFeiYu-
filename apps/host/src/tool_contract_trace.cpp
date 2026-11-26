#include "tool_contract_trace.hpp"
#include <dafeiyu/digest.hpp>
#include <dafeiyu/tool_catalog.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <set>

namespace dfy::app {
using Json = nlohmann::json;
// 将规范契约和模型实际描述对照，源位置指向原请求 input.tools 的零起索引。
// 步骤：选取同一代次的提交基线 → 逐项核对契约及绑定 → 标明当步退出的成员 → 暂存到提交回调。
Json ToolContractTrace::prepare(const agent::ModelCapture& capture) {
    pending_ = {};
    const auto& contracts = *capture.tool_contracts;
    const auto attempt = capture.attempt;
    auto next = committed_;
    Json result{{"epoch",capture.evidence_epoch},{"available",Json::array()},{"changes",Json::array()}};
    constexpr std::size_t limit = 256;
    const auto unseen = std::ranges::count_if(contracts,[&](const auto& item) { return !next.known.contains(item.id); });
    if (next.session!=attempt.step.run.session || next.epoch!=capture.evidence_epoch ||
        next.known.size()+static_cast<std::size_t>(unseen)>limit) {
        result["baseline"] = next.session==attempt.step.run.session && next.epoch==capture.evidence_epoch ? "capacity" : "history";
        next = {attempt.step.run.session,capture.evidence_epoch,{}};
    }
    if (contracts.size()!=capture.input->tools.size()) throw std::logic_error("trace: incomplete granted contract snapshot");
    std::set<std::string> active;
    for (std::size_t i=0; i<contracts.size(); ++i) {
        const auto& tool = contracts[i];
        const auto& sent = capture.input->tools[i];
        if (sent.name!=tool.model.name || sent.description!=tool.model.description || sent.parameters!=tool.model.parameters)
            throw std::logic_error("trace: contract and actual definition differ");
        active.insert(tool.id);
        const auto digest = platform::sha256(Json::array({tool.id,tool.version,tool.plugin_version.value_or(""),
            sent.name,sent.description,tool.schema_digest.value_or(""),tool.changes_state,tool.idempotent}).dump());
        auto [it,fresh] = next.known.try_emplace(tool.id);
        auto& entry = it->second;
        std::string state;
        if (fresh || entry.digest!=digest) {
            state = fresh ? "introduced" : "updated";
            entry.digest = digest;
            entry.origin = attempt;
            entry.index = i;
        } else if (entry.instance!=tool.instance) state = "rebound";
        else if (!entry.available) state = "entered";
        entry.instance = tool.instance;
        entry.available = true;
        if (!state.empty()) result["changes"].push_back({{"id",tool.id},{"state",state}});
        const auto& origin = entry.origin;
        result["available"].push_back({{"id",tool.id},{"contract",tool.version},{"digest",digest},{"instance",tool.instance},
            {"origin",{{"run",origin.step.run.sequence},{"step",origin.step.sequence},{"attempt",origin.sequence},{"toolIndex",entry.index}}}});
    }
    for (auto& [id,entry] : next.known) if (entry.available && !active.contains(id)) {
        entry.available = false;
        result["changes"].push_back({{"id",id},{"state","left"}});
    }
    next_ = std::move(next);
    pending_ = attempt;
    return result;
}
// 对应请求已被模型客户端接纳才保留发现记录，预备失败的轨迹不会消耗下次首次发现。
void ToolContractTrace::submitted(agent::AttemptId attempt) noexcept {
    if (pending_==attempt) { committed_ = std::move(next_); pending_ = {}; }
}
}
