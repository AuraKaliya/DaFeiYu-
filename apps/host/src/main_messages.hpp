#pragma once
#include "message_store.hpp"
#include <dafeiyu/run_identity.hpp>
#include "conversation.hpp"
#include <map>

namespace dfy::app {
class Application;
class SoftwareLog;
class RunLog;
// 固定原文与目的地，普通发送跨启动有效，追加与队列绑定原实例和回合。
nlohmann::json user_envelope(std::string_view route, const std::string& text, const std::string& instance,
    agent::RunId target, std::optional<std::int64_t> source_utc_ms = {});
// 业务线程持有投递责任，数据库先记录交付边界，再调用原应用入口。
class MainMessages : public std::enable_shared_from_this<MainMessages> {
  public:
    // 绑定独占目录的当前实例和原业务线程。
    MainMessages(Application& application, std::shared_ptr<MessageStore> store, std::string instance, SoftwareLog* log,
        RunLog* trace, bool structured);
    // 为面向用户的回合固定发言工具与渠道说明，保留已有的专用工具范围。
    bool prepare_output(agent::RunOptions& options);
    // 在每个主会话入口固定受众；主人私聊可按主题或稳定身份读取一份原件。
    void prepare_memory(agent::RunOptions& options, DialogueAudience audience, const std::string& prompt);
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
    // 取业务线程已有的实收与待写原件，为本次决策提供带版本的采样。
    agent::DecisionStatusSource delivery_status() const;
    // 使用原回合及渠道投递号保存发言，写盘失败时保留原件。
    nlohmann::json reply(agent::RunId run, const std::string& suffix, const std::string& text);
    // 按本轮的实际发言裁定是否需要另行交付最终文本。
    void final_reply(agent::RunId run, const std::string& text);
    // 提交原投递并以数据库消息号记录渠道实效。
    std::int64_t commit_reply(const nlohmann::json& envelope);
    Application& app_;
    std::shared_ptr<MessageStore> store_;
    std::string instance_;
    std::int64_t memory_revision_;
    std::string memory_binding_ = "main";
    std::optional<std::int64_t> incoming_memory_;
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
