#include <dafeiyu/session.hpp>
#include <nlohmann/json.hpp>

namespace dfy::agent {
namespace {
using Json = nlohmann::json;
// 生成一次宿主声明，三个范围共用同一组不可省略的计数字段。
std::string count_definitions() {
    const Json integer{{"type","integer"}};
    const Json counts{{"type","object"},{"properties",{{"proposed",integer},{"admitted",integer},{"dispatched",integer}}},
        {"required",{"proposed","admitted","dispatched"}},{"additionalProperties",false}};
    Json schema{{"type","object"},{"properties",{{"session",counts},{"turn",counts},{"response",counts},
        {"responseStep",integer},{"responseAttempt",integer}}},{"required",{"session","turn"}},{"additionalProperties",false}};
    return Json::array({{{"key","host.tool_counts"},{"version",1},{"description","同一有效会话、当前回合和上一份模型响应提出、宿主接纳及实际派发的工具数量。"},
        {"defaultAttention",{{"mode","implicit"}}},{"valueSchema",std::move(schema)}}}).dump();
}
// 保留各阶段独立的计数，零表示该范围已核实为零。
Json count_value(const ToolCallCounts& count) {
    return {{"proposed",count.proposed},{"admitted",count.admitted},{"dispatched",count.dispatched}};
}
}
// 对照原模型输出与去重后的执行器槽位，拒绝、失败或排队只计入实际经过的阶段。
ToolCallCounts TurnRequest::tool_counts(std::optional<AttemptId> response) const noexcept {
    auto counts = tools_ ? tools_->counts(response) : ToolCallCounts{};
    for (const auto& model : model_steps_)
        if (!response || model.attempt == *response) counts.proposed += model.proposed_tools;
    return counts;
}
// 在当前逻辑决策固定统计，已退出回合只计入同一绑定的会话总数。
// 步骤：读取当轮已发生事实 → 合并会话总量 → 关联最近有效模型响应并签发采样。
DecisionStatusSource TurnRequest::tool_count_status(ToolCallCounts previous, std::uint64_t step) const {
    static const auto definitions = count_definitions();
    const auto current = tool_counts();
    previous += current;
    Json value{{"session",count_value(previous)},{"turn",count_value(current)}};
    if (!model_steps_.empty() && model_steps_.back().result == runtime::OperationState::succeeded) {
        const auto& attempt = model_steps_.back().attempt;
        value["response"] = count_value(tool_counts(attempt));
        value["responseStep"] = attempt.step.sequence;
        value["responseAttempt"] = attempt.sequence;
    }
    return {"host",definitions,Json::array({{{"key","host.tool_counts"},{"version",1},{"subject","turn/"+std::to_string(turn())},
        {"fact",step},{"value",std::move(value)}}}).dump()};
}
}
