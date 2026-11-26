#pragma once
#include <dafeiyu/plan_definition.hpp>
#include <dafeiyu/plan_book.hpp>

namespace dfy::agent {
struct PlanPrompt {
    PlanBinding expected;
    model::Request request;
};
// 为显式规划生成无工具执行入口的模型请求，旧计划须与宿主期望版本一致。
PlanPrompt prepare_plan_prompt(const PlanScope& scope, std::uint64_t expected_revision, std::string_view goal,
    const PlanDefinition* previous = nullptr, std::size_t completed_prefix = 0, std::string_view guidance = {});
// 将宿主选定的观察、工具回执及步骤验证附入规划数据，保持可执行工具为空。
void project_plan_evidence(model::Request& plan, const std::vector<model::Message>& history, const PlanSnapshot& state,
    std::string_view status = {}, std::span<const std::string> observations = {}, std::optional<bool> action_basis_current = {},
    std::size_t goal_message = 0);
// 只解析完整助手文本中的声明式候选，运行、目标和新修订由原宿主上下文绑定。
PlanDefinition parse_plan_response(const PlanScope& scope, PlanBinding expected, const model::Response& response);
// 编码已校验计划中的一个步骤为数据，版本和工具 ID 不构成新的执行授权。
std::string plan_step_input(const PlanDefinition& definition, std::size_t index);
}
