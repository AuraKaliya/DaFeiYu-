#pragma once
#include <dafeiyu/tool_catalog.hpp>

namespace dfy::agent {
// 计划步骤是业务子目标，与一次模型请求的 StepId 不同；完成条件这里只保存声明，不作为证据。
struct PlanStepDefinition {
    std::string id, objective, completion_criterion;
    std::vector<std::string> tools;
    // 比较完整步骤定义，已执行步骤不能通过改正文或工具集偷偷重写。
    bool operator==(const PlanStepDefinition&) const = default;
};
struct PlanBinding {
    RunId run;
    std::uint64_t goal_revision = 0, plan_revision = 0;
    // 比较原运行及目标、计划修订，局部编号不能跨运行复用。
    bool operator==(const PlanBinding&) const = default;
};
struct PlanLimits {
    std::size_t maximum_steps = 16, maximum_revisions = 4, maximum_text_bytes = 16*1024;
};
// 宿主固定的规划范围，只保留候选可引用的工具 ID，不取得任何执行工厂或 DLL 凭证。
class PlanScope {
  public:
    // 从原运行范围固定工具清单和限额；空清单仍允许纯文本步骤。
    PlanScope(RunId run, std::uint64_t goal_revision, const std::optional<ToolSnapshot>& tools = {}, PlanLimits limits = {});
    // 返回原运行身份，不签发新业务操作。
    RunId run() const noexcept { return run_; }
    // 返回宿主固定的目标修订，模型正文不能更新。
    std::uint64_t goal_revision() const noexcept { return goal_revision_; }
    // 返回固定限额，不根据模型请求扩大。
    PlanLimits limits() const noexcept { return limits_; }
    // 借用可引用的工具 ID；它们仍须经过原执行入口的当前授权检查。
    const std::vector<std::string>& tools() const noexcept { return tools_; }
    // 判断候选工具是否属于原规划范围，不实施调用或重新绑定实例。
    bool permits(std::string_view id) const noexcept;
  private:
    RunId run_;
    std::uint64_t goal_revision_;
    PlanLimits limits_;
    std::vector<std::string> tools_;
};
// 校验后的不可变顺序计划，没有运行状态、完成权或执行入口。
class PlanDefinition {
  public:
    // 在完整校验后构造计划，修订由宿主传入，不从模型字段授予。
    static PlanDefinition create(const PlanScope& scope, std::uint64_t revision, std::vector<PlanStepDefinition> steps);
    // 复制不可变候选值，不复制任何执行责任。
    PlanDefinition(const PlanDefinition&) = default;
    // 禁止赋值覆盖已发布的版本。
    PlanDefinition& operator=(const PlanDefinition&) = delete;
    // 返回完整运行及版本绑定。
    PlanBinding binding() const noexcept { return binding_; }
    // 借用原顺序步骤，没有隐式依赖图或可执行表达式。
    const std::vector<PlanStepDefinition>& steps() const noexcept { return steps_; }
    // 返回所有字段按出现次数计入的逻辑字节数。
    std::size_t text_bytes() const noexcept { return bytes_; }
  private:
    // 接收校验后的完整候选，不发布部分步骤。
    PlanDefinition(PlanBinding binding, std::vector<PlanStepDefinition> steps, std::size_t bytes)
        : binding_(binding),steps_(std::move(steps)),bytes_(bytes) {}
    PlanBinding binding_;
    const std::vector<PlanStepDefinition> steps_;
    const std::size_t bytes_;
};
}
