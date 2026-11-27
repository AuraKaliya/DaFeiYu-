#pragma once
#include "message_store.hpp"
#include <dafeiyu/run_identity.hpp>
#include "conversation.hpp"
#include <map>
#include <deque>

namespace dfy::app {
class Application;
class SoftwareLog;
class RunLog;
class TaskReflection;
// 固定原文与目的地，普通发送跨启动有效，追加与队列绑定原实例和回合。
nlohmann::json user_envelope(std::string_view route, const std::string& text, const std::string& instance,
    agent::RunId target, std::optional<std::int64_t> source_utc_ms = {});
// 业务线程持有投递责任，数据库先记录交付边界，再调用原应用入口。
class MainMessages : public std::enable_shared_from_this<MainMessages> {
  public:
    // 绑定独占目录的当前实例和原业务线程。
    MainMessages(Application& application, std::shared_ptr<MessageStore> store, std::string instance, SoftwareLog* log,
        RunLog* trace, bool structured, std::shared_ptr<TaskReflection> reflection);
    // 为面向用户的回合固定发言工具与渠道说明，保留已有的专用工具范围。
    bool prepare_output(agent::RunOptions& options, DialogueAudience audience, const std::string& prompt);
    // 在每个主会话入口固定受众；主人私聊可按主题或稳定身份读取一份原件。
    void prepare_memory(agent::RunOptions& options, DialogueAudience audience, const std::string& prompt);
    // 绑定本体直接接纳的工具任务，子会话与信箱转述无此入口。
    void watch_task(const agent::TurnDiagnostics& run, const std::string& goal, const std::string& tool,
        runtime::DomainId provider, const agent::ToolSnapshot& scope, std::optional<std::string> expected = {});
    // 只在原运行实际退出后固化计划、工具动作与独立的验证回执。
    void task_exited(const agent::TurnRequest& run);
    // 前台工作及信箱优先，空闲推进最多整理一份仍获准的任务亲历。
    void review_idle_task(bool idle);
    // 收尾时等待空闲推断实际退出。
    bool close_reflection();
    // 管理终端查询可收录的最近原话身份。
    std::string last_memory_source() const { return store_->last_memory_source(); }
    // 仅受信管理入口可收录或撤回当前档案原件的读取权。
    void change_memory(std::string_view identity, const std::optional<std::string>& topic);
    // 明确清空会话后隔离旧回合的投影，旧投递仍按原号结算。
    void clear_context(std::uint64_t through) noexcept { context_floor_ = through; delivered_.reset(); }
    // 绑定业务线程的实时终端输出，只有首次写盘成功才通知。
    void on_reply(std::function<void(const std::string&)> sink) { reply_sink_ = std::move(sink); }
    // 查询原回合仍在等待写入的一项投递号，供发起终端等待或说明结果。
    std::string pending_reply(agent::RunId run) const;
    // 将已经入库且尚未投递的原文交付给其指定入口，保留独立的实际回执。
    nlohmann::json deliver(std::int64_t id, const nlohmann::json& envelope, std::int64_t stored_utc_ms);
    // 在终端命令空间按原号入库并交付普通输入。
    void terminal(const std::string& submission, const std::string& text);
    // 重试已知结果的写盘，不重发已执行的动作。
    void flush();
    // 启动时区分未交付、可恢复队列和实效未知的旧运行。
    void restore();
    // 业务收尾时完成已核实结果的写盘，无法持久化则报告收尾失败。
    void finish();
    // 将存储失败限定为软件事件，正文及 SQL 不进入诊断。
    void storage_failed() const noexcept;
    // 在实际启动待发项前固定不可随意重放的交付边界。
    void start_queue(std::uint64_t native);
    // 关联从队首开始的新回合与原始用户消息号。
    void started_queue(std::uint64_t native, agent::RunId run);
    // 在移除等待项前将该消息的移除事实写盘。
    void remove_queue(std::uint64_t native);
    // 根据原运行收件与唯一结算登记实际送模步骤和面向用户的回答。
    void observe(agent::RunId run, const std::vector<agent::TurnInput>& inputs, const CompletedTurn* completed);
  private:
    // 保存最新已核实的投递状态，暂时写盘失败时保留原消息号。
    void publish(std::int64_t id, const nlohmann::json& receipt);
    // 校验一次显式发言，将实际写入状态交还原工具调用。
    std::unique_ptr<agent::ToolWork> speak(const agent::ToolInvocation& invocation);
    // 注册主人私聊中的记忆判断与按需检索契约。
    void register_memory_tools();
    // 注册私聊中实际作出选择和比较已见亲历的两个入口。
    void register_experience_tools();
    // 将亲历选择作为真实答复交付，当前原话与投递一起核实。
    std::unique_ptr<agent::ToolWork> choose(const agent::ToolInvocation& invocation);
    // 只比较本轮已查回的独立经历。
    std::unique_ptr<agent::ToolWork> review_experience(const agent::ToolInvocation& invocation);
    // 投影亲历及其解释，历史快照只保留本次读取的数量。
    agent::DecisionStatusSource experience_source(std::uint64_t turn, const nlohmann::json& selection) const;
    // 投影本轮获准读取的任务、原行动或原回执，禁止历史快照重新展开。
    agent::DecisionStatusSource task_source(std::uint64_t turn, const nlohmann::json& selection) const;
    // 把本体写下的解释和带角色的任务证据分别装入当前私聊。
    agent::DecisionStatusSource understanding_source(std::uint64_t turn, const nlohmann::json& selection) const;
    // 封装记忆工具的确定回执及效果。
    static std::unique_ptr<agent::ToolWork> memory_receipt(nlohmann::json value, agent::ToolEffect effect, bool ok = true);
    // 核实原回合与实际收件后，根据模型判断收录当前原话。
    std::unique_ptr<agent::ToolWork> consider_memory(const agent::ToolInvocation& invocation);
    // 每轮只提交一批检索证据，工具回执只携带数量及截断状态。
    std::unique_ptr<agent::ToolWork> search_memory(const agent::ToolInvocation& invocation);
    // 仅用本轮上一决策可见的两份原件提交复盘关系。
    std::unique_ptr<agent::ToolWork> reflect_memory(const agent::ToolInvocation& invocation);
    // 取业务线程已有的实收与待写原件，为本次决策提供带版本的采样。
    agent::DecisionStatusSource delivery_status() const;
    // 使用原回合及渠道投递号保存发言，写盘失败时保留原件。
    nlohmann::json reply(agent::RunId run, const std::string& suffix, const std::string& text, const nlohmann::json& experience = nullptr);
    // 按本轮的实际发言裁定是否需要另行交付最终文本。
    void final_reply(agent::RunId run, const std::string& text);
    // 提交原投递并以数据库消息号记录渠道实效。
    std::int64_t commit_reply(const nlohmann::json& envelope);
    Application& app_;
    std::shared_ptr<MessageStore> store_;
    std::string instance_;
    std::int64_t memory_revision_;
    std::string memory_binding_ = "main";
    bool owner_memory_turn_ = false;
    std::optional<std::int64_t> incoming_memory_;
    std::optional<agent::DecisionStatusSource> search_result_;
    std::optional<std::pair<agent::RunId,nlohmann::json>> watched_task_;
    std::deque<nlohmann::json> unsaved_tasks_;
    bool idle_review_due_ = true;
    std::shared_ptr<TaskReflection> reflection_;
    std::uint64_t search_step_ = 0;
    SoftwareLog* log_;
    RunLog* trace_;
    std::function<void(const std::string&)> reply_sink_;
    std::unique_ptr<agent::ToolCatalog> reply_tools_;
    std::map<std::string,nlohmann::json> explicit_deliveries_;
    std::uint64_t output_input_ = 0;
    std::optional<StoredMessage> delivered_;
    std::uint64_t context_floor_ = 0;
    std::map<std::int64_t,nlohmann::json> retry_;
    std::map<std::uint64_t,std::int64_t> queued_;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::int64_t> appended_;
    std::pair<agent::RunId,std::int64_t> parent_{};
    std::map<std::string,nlohmann::json> replies_;
    struct PendingInput { nlohmann::json envelope; std::int64_t stored_utc_ms; };
    std::map<std::int64_t,PendingInput> pending_;
    std::chrono::steady_clock::time_point retry_at_{};
};
}
