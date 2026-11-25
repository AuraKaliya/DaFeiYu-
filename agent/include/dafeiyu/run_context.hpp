#pragma once
#include <dafeiyu/tool_work.hpp>
#include <dafeiyu/turn_input.hpp>
#include <span>

namespace dfy::agent {
using MessageHistory = std::vector<model::Message>;
// 单次运行的上下文事务；完成的工具结果可独立提交，不依赖后续最终回答成功。
class RunContext {
  public:
    static constexpr std::size_t maximum_reply_bytes = 64*1024+256;
    // 从历史创建目标及最多四份观察数据，观察仅按数据消息附加，不授予角色或工具权限。
    RunContext(std::shared_ptr<const MessageHistory> history, std::string prompt, std::vector<std::string> observations = {},
        std::string status = {});
    // 禁止复制未提交事务，避免两个写入者共享同一待填上下文。
    RunContext(const RunContext&) = delete;
    // 禁止赋值或移动覆盖事务责任，历史快照另行按值持有。
    RunContext& operator=(const RunContext&) = delete;
    // 借用下一模型请求的完整消息；工具结果尚未提交时拒绝，下一次变更前有效。
    const MessageHistory& messages() const;
    // 返回不可变的已提交消息持有，不包含未完成的工具占位。
    std::shared_ptr<const MessageHistory> committed() const noexcept { return committed_; }
    // 在动作前预留整批结果正文并核对完整请求预算，失败不发布半个批次。
    void prepare_tools(const model::Message& assistant, const std::vector<model::ToolDefinition>& definitions);
    // 在预留空间内格式化终态及真实效果，不再分配结果正文；拒绝状态与实际执行分开。
    void set_tool_result(std::size_t index, const ToolResult& outcome, runtime::OperationState state,
        ToolError fault = ToolError::none, bool accepted = true);
    // 仅在全部结果已准备时发布本批历史，提交不分配内存。
    void commit_tools();
    // 在完整步骤后准备用户补充和可选阶段答复，保持已提交历史独立。
    void append_user_inputs(std::span<const TurnInput> inputs, const model::Message* prior_answer);
    // 在无未完成工具时追加有界步骤数据，准备失败不改变已提交历史或当前输入。
    void begin_step(std::string data);
    // 在无未完成工具时追加宿主明确更新的用户目标，保留旧目标及动作事实供解释。
    void revise_goal(std::uint64_t revision, std::string goal);
    // 在无未完成工具时追加新观察封套，不删除旧输入、动作或效果；准备失败保持原上下文。
    void refresh_observations(runtime::DomainId binding, std::vector<std::string> observations);
    // 提交一个步骤的模型文本并保留后续输入入口；文本不表示宿主已验证目标。
    void commit_step_text(std::string answer, std::optional<std::string> reasoning = {});
    // 准备并提交最终文本；失败不会回滚已提交工具事实。
    void finish_text(std::string answer, std::optional<std::string> reasoning = {});
    // 丢弃未提交的纯文本输入并封闭事务；不允许丢弃尚待结果的工具批次。
    void seal();
  private:
    std::shared_ptr<const MessageHistory> committed_;
    std::shared_ptr<MessageHistory> pending_;
    std::vector<bool> replies_;
    std::size_t tool_begin_ = 0;
    bool finished_ = false;
};
}
