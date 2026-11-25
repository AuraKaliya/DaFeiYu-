#include "companion_visit.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 将原世界、玩家消息号和语义地标绑定为一份准确参数。
std::string visit_arguments(const CompanionVisit& visit) {
    return nlohmann::json{{"generation", visit.address.world}, {"order", visit.order}, {"place", visit.place}}.dump();
}

// 从宿主工具而非模型文字收取实际观察，保留领域插件的证据原文。
// 步骤：核对原回合与唯一候选 → 核对执行和实际退出 → 校验来源并分别记录到达和发现。
void verify_visit(CompanionVisit& visit, const agent::TurnRequest& turn, const std::string& tool) {
    visit.phase = VisitPhase::failed;
    if (!visit.run || *visit.run != turn.run_id()) return;
    const auto steps = turn.tool_steps();
    if (steps.size() != 1 || steps[0].tool_id != tool || !steps[0].accepted ||
        steps[0].result != runtime::OperationState::succeeded || steps[0].exit != runtime::OperationState::succeeded ||
        steps[0].fault != agent::ToolError::none) return;
    auto result = turn.tool_result(steps[0].candidate);
    if (!result || !result->succeeded || result->output.size() > 16384) return;
    const auto receipt = nlohmann::json::parse(result->output, nullptr, false);
    if (!receipt.is_object() || !receipt.contains("generation") || receipt["generation"] != visit.address.world ||
        !receipt.contains("order") || receipt["order"] != visit.order || !receipt.contains("place") || receipt["place"] != visit.place ||
        !receipt.contains("phase") || receipt["phase"] != "arrived" || !receipt.contains("exited") || receipt["exited"] != true ||
        !receipt.contains("observed") || !receipt["observed"].is_boolean()) return;
    visit.evidence = std::move(result->output);
    visit.phase = receipt["observed"].get<bool>() ? VisitPhase::observed : VisitPhase::arrived;
}
}
