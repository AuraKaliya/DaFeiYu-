#pragma once
#include <dafeiyu/async_model.hpp>
#include <dafeiyu/hooks.hpp>

namespace dfy::agent {
class Session;
struct TurnNotifications { std::optional<events::ObservationReceipt> started, finished; };
// 宿主线程内的一轮问答结果，业务终态、通知回执与后台退出分别保留。
class TurnRequest {
  public:
    // 禁止复制问答身份和完成权。
    TurnRequest(const TurnRequest&) = delete;
    // 禁止赋值或隐式移动已接受回合。
    TurnRequest& operator=(const TurnRequest&) = delete;
    // 返回本会话接受时固定的回合号。
    std::uint64_t turn() const noexcept { return turn_; }
    // 返回问答业务状态，Succeeded 表示完整问答已提交历史。
    std::shared_ptr<const runtime::Operation> operation() const noexcept { return result_.operation(); }
    // 返回本轮后台资源已退出确认，取消业务不提前完成此操作。
    std::shared_ptr<const runtime::Operation> exit() const noexcept { return exit_.operation(); }
    // 请求取消当前回合，提交前由宿主推进裁定；已提交成功不回滚。
    runtime::CancelDisposition request_cancel(runtime::CancelReason reason) { return result_.request_cancel(reason); }
    // 借用成功回答，失败重抛原错误；取消和尚未完成明确拒绝。
    const std::string& answer() const;
    // 返回观察接纳回执副本，观察失败不会改写业务结果。
    TurnNotifications notifications() const noexcept { return notifications_; }
  private:
    friend class Session;
    // 准备尚未发布的回合记录，不调用模型或观察者。
    explicit TurnRequest(std::uint64_t turn) : turn_(turn) {}
    const std::uint64_t turn_;
    runtime::OperationSource result_{true}, exit_;
    std::string answer_;
    std::exception_ptr error_;
    TurnNotifications notifications_;
};

// 最小异步会话，只维护当前上下文；不引入工具循环、自动规划或会话持久化。
class Session {
  public:
    // 固定结果槽、共享事件通道与模型调用，历史正文总量最多 1 MiB。
    Session(model::Config config, std::shared_ptr<events::BroadcastChannel> events,
        std::size_t result_capacity = 4, model::Completion completion = {});
    // 取消并等待仍在使用会话输入的模型调用退出，外部回合结果继续有效。
    ~Session();
    // 禁止复制会话身份、历史及后台责任。
    Session(const Session&) = delete;
    // 禁止赋值或隐式移动进行中的会话。
    Session& operator=(const Session&) = delete;
    // 空闲时接受一轮问答，完整准备和模型提交成功后才发布开始通知。
    std::shared_ptr<TurnRequest> begin(const std::string& prompt);
    // 非阻塞推进取消、完整回答及原子历史提交；通知仅进入事件队列。
    void advance();
    // 空闲时清空已提交历史并通知，保持会话身份及回合号。
    events::ObservationReceipt clear();
    // 停止新回合并请求取消，等待实际退出期间仍可推进事件和管理操作。
    void close();
    // 返回是否有尚未实际退出的回合。
    bool busy() const noexcept { return static_cast<bool>(active_); }
    // 判断已经停止接纳且后台调用真正退出。
    bool closed() const noexcept { return !accepting_ && !active_ && models_.closed(); }
    // 返回宿主签发的独立会话身份。
    runtime::DomainId id() const noexcept { return id_; }
    // 返回已提交消息数，成功问答成对提交，失败和取消不改变历史。
    std::size_t message_count() const noexcept { return history_.size(); }
  private:
    // 构造不可变会话事实并返回独立接纳回执，任何观察失败都留在通知边界。
    events::ObservationReceipt notify(std::string_view kind, std::uint64_t turn) noexcept;
    // 回收无客户端持有的已退出回合，保留结果继续计入槽容量。
    void collect() noexcept;
    model::Config config_;
    std::shared_ptr<events::BroadcastChannel> events_;
    std::shared_ptr<const runtime::StructDescriptor> descriptor_;
    const runtime::DomainId id_;
    std::uint64_t turn_ = 0;
    std::vector<std::shared_ptr<TurnRequest>> records_;
    std::shared_ptr<TurnRequest> active_;
    std::shared_ptr<model::ModelRequest> model_request_;
    std::vector<model::Message> history_, pending_;
    model::AsyncClient models_;
    bool accepting_ = true;
};
}
