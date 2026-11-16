#pragma once
#include <dafeiyu/plan_definition.hpp>
#include <dafeiyu/operation.hpp>

namespace dfy::agent {
struct PlanStepKey {
    PlanBinding plan;
    std::string step;
    // 比较原运行、目标、计划版本与步骤，局部 ID 不构成完整身份。
    bool operator==(const PlanStepKey&) const = default;
};
enum class PlanStepState { pending, running, completed, failed, cancelled, skipped };
enum class PlanWait { none, work, exit, verification, stopped };
// 宿主关联的原工作读句柄，计划不接管其完成权或取消责任。
struct PlanWork {
    AttemptId execution;
    std::shared_ptr<const runtime::Operation> result, exit;
};
struct PlanStepView {
    PlanStepKey key;
    PlanStepState state = PlanStepState::pending;
    PlanWait waiting = PlanWait::none;
    std::optional<AttemptId> execution;
    std::optional<runtime::OperationState> result, exit;
    std::string evidence;
};
struct PlanSnapshot {
    std::shared_ptr<const PlanDefinition> definition;
    std::vector<PlanStepView> steps;
    bool stopped = false;
};
// 同宿主线程的顺序计划记录，只投影原工作并接收显式宿主验证，不自行执行步骤或结算 Run。
class PlanBook {
  public:
    // 固定规划范围，尚无候选时计划修订为零。
    explicit PlanBook(PlanScope scope) : scope_(std::move(scope)) {}
    // 禁止复制计划推进及验证责任。
    PlanBook(const PlanBook&) = delete;
    // 禁止赋值或移动覆盖正在使用的计划。
    PlanBook& operator=(const PlanBook&) = delete;
    // 返回当前候选的完整绑定，无候选时只使用宿主运行与目标身份。
    PlanBinding binding() const noexcept;
    // 在步骤边界安装有限新版本，已完成／跳过的前缀必须逐字段保持原样。
    void install(PlanBinding expected, std::vector<PlanStepDefinition> proposal);
    // 只给下一待执行步骤关联原工作，调用不会启动工具或模型。
    void start(const PlanStepKey& key, PlanWork work);
    // 宿主确认完成条件已验证；原工作必须成功并实际退出，重复同证据不重复推进。
    void confirm(const PlanStepKey& key, std::string_view evidence);
    // 宿主明确跳过下一未开始步骤，模型候选中没有这个控制入口。
    void skip(const PlanStepKey& key, std::string_view reason);
    // 阻止新计划和步骤，原工作仍由其原运行所有者取消及收尾。
    void stop() noexcept { stopped_ = true; }
    // 读取原操作状态及计划验证事实，查询不推进任何工作。
    PlanSnapshot snapshot() const;
  private:
    struct Entry {
        PlanStepState state = PlanStepState::pending;
        std::optional<PlanBinding> started;
        std::optional<PlanWork> work;
        std::string evidence;
    };
    // 按当前定义中的精确步骤 ID 查找，不按标题或相似内容回退。
    std::size_t find(std::string_view id) const;
    // 返回第一个未确认完成或跳过的步骤，失败及等待也会阻止后续步骤。
    std::size_t next() const noexcept;
    // 核对宿主证据或跳过原因的有界文本，不把文本本身解释为代码。
    static void validate_evidence(std::string_view evidence);
    const PlanScope scope_;
    std::shared_ptr<const PlanDefinition> definition_;
    std::vector<Entry> entries_;
    bool stopped_ = false;
};
}
