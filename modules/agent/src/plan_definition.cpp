#include <dafeiyu/plan_definition.hpp>
#include <dafeiyu/utf8.hpp>
#include <algorithm>
#include <set>
#include <stdexcept>

namespace dfy::agent {
namespace {
// 检查可读步骤正文有界、有效且非空白，不把正文解释为命令或完成证据。
void text_field(const std::string& value) {
    if (value.empty() || value.size() > 2048 || !runtime::valid_utf8(value) || value.find_first_not_of(" \t\r\n") == std::string::npos)
        throw std::invalid_argument("plan: bounded nonblank text required");
}
// 限制步骤 ID 为有界局部标识，避免路径、空白或任意正文充当身份。
void step_id(const std::string& value) {
    if (value.empty() || value.size() > 64) throw std::invalid_argument("plan: invalid step ID");
    for (const auto c : value) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
        throw std::invalid_argument("plan: invalid step ID");
}
}
// 固定原运行和有限规划范围，工具失效不会由新计划补成新授权。
// 步骤：校验身份及硬上限 → 复制当前可用工具 ID → 固定宿主目标版本。
PlanScope::PlanScope(RunId run, std::uint64_t goal_revision, const std::optional<ToolSnapshot>& tools, PlanLimits limits)
    : run_(run),goal_revision_(goal_revision),limits_(limits) {
    if (!run.session || !run.sequence || !goal_revision || !limits.maximum_steps || limits.maximum_steps > 32 ||
        !limits.maximum_revisions || limits.maximum_revisions > 16 || !limits.maximum_text_bytes || limits.maximum_text_bytes > 64*1024)
        throw std::invalid_argument("plan: invalid scope or limits");
    if (tools) for (const auto& tool : tools->descriptions()) tools_.push_back(tool.id);
}
// 只读取原工具清单，存在于计划不等于当前仍有执行权限。
bool PlanScope::permits(std::string_view id) const noexcept { return std::find(tools_.begin(),tools_.end(),id) != tools_.end(); }
// 校验完整候选后发布不可变定义，不接受状态、目标修改或隐式能力扩张。
// 步骤：核对版本和步骤数量 → 校验唯一身份及正文 → 核对工具子集与总预算 → 发布原顺序。
PlanDefinition PlanDefinition::create(const PlanScope& scope, std::uint64_t revision, std::vector<PlanStepDefinition> steps) {
    const auto limits = scope.limits();
    if (!revision || revision > limits.maximum_revisions || steps.empty() || steps.size() > limits.maximum_steps)
        throw std::invalid_argument("plan: invalid revision or step count");
    std::set<std::string> ids; std::size_t bytes = 0;
    for (const auto& step : steps) {
        step_id(step.id); text_field(step.objective); text_field(step.completion_criterion);
        if (!ids.insert(step.id).second || step.tools.size() > 8) throw std::invalid_argument("plan: duplicate step or tool count exceeded");
        bytes += step.id.size()+step.objective.size()+step.completion_criterion.size();
        std::set<std::string> tools;
        for (const auto& id : step.tools) {
            if (!scope.permits(id) || !tools.insert(id).second) throw std::invalid_argument("plan: tool outside fixed scope or duplicated");
            bytes += id.size();
        }
        if (bytes > limits.maximum_text_bytes) throw std::length_error("plan: text budget exhausted");
    }
    return {{scope.run(),scope.goal_revision(),revision},std::move(steps),bytes};
}
}
