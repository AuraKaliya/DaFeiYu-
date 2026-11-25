#pragma once
#include "message_store.hpp"
#include <dafeiyu/run_identity.hpp>
#include "conversation.hpp"
#include <map>

namespace dfy::app {
class Application;
class SoftwareLog;
// 固定原文与目的地，普通发送跨启动有效，追加与队列绑定原实例和回合。
nlohmann::json user_envelope(std::string_view route, const std::string& text, const std::string& instance,
    agent::RunId target, std::optional<std::int64_t> source_utc_ms = {});
// 业务线程持有投递责任，数据库先记录交付边界，再调用原应用入口。
class MainMessages {
  public:
    // 绑定独占目录的当前实例和原业务线程。
    MainMessages(Application& application, std::shared_ptr<MessageStore> store, std::string instance, SoftwareLog* log)
        : app_(application), store_(std::move(store)), instance_(std::move(instance)), log_(log) {}
    // 将已经入库且尚未投递的原文交付给其指定入口，保留独立的实际回执。
    nlohmann::json deliver(std::int64_t id, const nlohmann::json& envelope);
    // 在终端命令空间按原号入库并交付普通输入。
    void terminal(const std::string& submission, const std::string& text);
    // 重试已知结果的写盘，不重发已执行的动作。
    void flush();
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
    Application& app_;
    std::shared_ptr<MessageStore> store_;
    std::string instance_;
    SoftwareLog* log_;
    std::map<std::int64_t,nlohmann::json> retry_;
    std::map<std::uint64_t,std::int64_t> queued_;
    std::map<std::pair<std::uint64_t,std::uint64_t>,std::int64_t> appended_;
    std::pair<agent::RunId,std::int64_t> parent_{};
    std::map<std::string,nlohmann::json> replies_;
    std::chrono::steady_clock::time_point retry_at_{};
};
}
