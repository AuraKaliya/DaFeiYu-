#pragma once
#include <dafeiyu/plan_definition.hpp>

namespace dfy::agent {
struct PlanPrompt {
    PlanBinding expected;
    model::Request request;
};
// 为显式规划生成无工具执行入口的模型请求，旧计划须与宿主期望版本一致。
PlanPrompt prepare_plan_prompt(const PlanScope& scope, std::uint64_t expected_revision, std::string_view goal, const PlanDefinition* previous = nullptr);
// 只解析完整助手文本中的声明式候选，运行、目标和新修订由原宿主上下文绑定。
PlanDefinition parse_plan_response(const PlanScope& scope, PlanBinding expected, const model::Response& response);
// 编码已校验计划中的一个步骤为数据，版本和工具 ID 不构成新的执行授权。
std::string plan_step_input(const PlanDefinition& definition, std::size_t index);
}
