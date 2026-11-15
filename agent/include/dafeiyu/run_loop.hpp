#pragma once
#include <dafeiyu/run_options.hpp>
#include <dafeiyu/run_context.hpp>
#include <dafeiyu/async_model.hpp>

namespace dfy::agent {
class TurnRequest;
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
