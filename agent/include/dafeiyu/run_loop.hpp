#pragma once
#include <dafeiyu/run_options.hpp>
#include <dafeiyu/run_context.hpp>
#include <dafeiyu/async_model.hpp>
#include <dafeiyu/plan_feedback.hpp>

namespace dfy::agent {
class TurnRequest;
struct PlanStepKey;
struct RunLoopState;
// 一次运行的有界协调器，结果仍由原 TurnRequest 的 Operation 提供。
class RunLoop {
  public:
    // 完整准备状态、上下文和限额后提交首个模型请求，失败不交付半个运行。
    RunLoop(RunId run, model::Config config, model::Backend backend, std::shared_ptr<const MessageHistory> history,
        std::string prompt, RunOptions options = {});
    // 请求停止并推进一次，未退出状态转入有界保留链而不丢失工作责任。
    ~RunLoop();
    // 禁止复制运行协调责任。
    RunLoop(const RunLoop&) = delete;
    // 禁止赋值或移动活动协调器。
    RunLoop& operator=(const RunLoop&) = delete;
    // 返回原回合结果持有，不转交模型或工具的推进责任。
    std::shared_ptr<TurnRequest> request() const noexcept;
    // 返回已提交上下文快照，不包含工具占位或未确认结果。
    std::shared_ptr<const MessageHistory> history() const noexcept;
    // 推进一个有界阶段，工具批次准备后让出宿主，不递归调用模型。
    void advance();
    // 在宿主控制入口决定原候选；拒绝重入，命令处理不推进工具工作。
    ConfirmationReceipt decide(const ConfirmationCaller& caller, const ConfirmationCommand& command);
    // 接收宿主对原步骤的验证，重查停止边界，不在确认调用中调度新模型或工具。
    void confirm_plan_step(const ConfirmationCaller& caller, const PlanStepKey& key, std::string_view evidence);
    // 接纳原目标内的有限重规划请求，只有后续推进才调用模型。
    PlanBinding replan(const ConfirmationCaller& caller, PlanBinding expected, std::string_view guidance);
    // 只在无在途模型和工具的边界更新目标，旧计划停止并保留，新规划延后提交。
    PlanBinding revise_goal(const ConfirmationCaller& caller, PlanBinding expected, std::string_view goal);
    // 在原步骤或明确工具实际退出后准备反馈请求，不认可已有采样为行动后证据。
    PlanFeedbackTicket request_plan_feedback(const ConfirmationCaller& caller, const PlanStepKey& key,
        ObservationGuard baseline, std::string field, runtime::Value expected, runtime::DomainId completed_tool = 0);
    // 用原反馈请求检查新观察，满足时只记录步骤验证，后续推进再继续运行。
    ObservationAssessment verify_plan_feedback(const ConfirmationCaller& caller, const PlanFeedbackTicket& ticket, const ObservationGuard& current);
    // 仅用于外层关闭时等待已有模型调用返回，工具清理仍按原协议推进。
    void wait_model();
    // 返回仍需要宿主重试收尾的运行数。
    static std::size_t retained() noexcept;
    // 在原宿主线程重试保留运行，不恢复新步骤或重放已发生动作。
    static std::size_t retry_retained();
  private:
    std::unique_ptr<RunLoopState> state_;
};
}
