#pragma once
#include <dafeiyu/subscription_queue.hpp>
#include <dafeiyu/operation.hpp>

namespace dfy::events {
enum class ReadOutcome { event, closed, cancelled };
struct ReadSnapshot { ReadOutcome outcome; std::optional<CloseMode> close_mode; };
class SubscriptionReader;

// 一次类型化读取；状态视图可独立保留，结果及借用须保持整个请求对象。
class ReadRequest {
  public:
    // 禁止复制读取身份和完成权。
    ReadRequest(const ReadRequest&) = delete;
    // 禁止赋值或隐式移动读取记录。
    ReadRequest& operator=(const ReadRequest&) = delete;
    // 取得只读操作状态视图，不转交结果所有权。
    std::shared_ptr<const runtime::Operation> operation() const noexcept { return source_.operation(); }
    // 请求取消本次读取，宿主下次推进时裁定，不取消队列或生产者业务。
    runtime::CancelDisposition request_cancel(runtime::CancelReason reason) { return source_.request_cancel(reason); }
    // 读取已结算结果副本，Pending 时拒绝，不推进工作。
    ReadSnapshot result() const;
    // 在本请求持有期间借用已读事件，非事件结果拒绝。
    const EventRecord& event() const;
    // 显式取得事件的独立数据持有，受事件存储预算限制。
    EventLease retain_event() const;
  private:
    friend class SubscriptionReader;
    // 创建支持协作取消的 Pending 读取，不触碰订阅队列。
    ReadRequest() = default;
    runtime::OperationSource source_{true};
    std::optional<ReadSnapshot> result_;
    EventLease event_;
};

// 同宿主线程的有界读请求控制器；每个控制器至多一项 Pending，结果保留也占容量。
class SubscriptionReader {
  public:
    // 绑定非空队列并建立正请求容量，不创建后台线程。
    SubscriptionReader(std::shared_ptr<SubscriptionQueue> queue, std::size_t request_capacity);
    // 结算尚未结束的读取为取消，不关闭队列或改写已经完成的结果。
    ~SubscriptionReader();
    // 禁止复制待完成读取责任。
    SubscriptionReader(const SubscriptionReader&) = delete;
    // 禁止赋值或隐式移动读取控制器。
    SubscriptionReader& operator=(const SubscriptionReader&) = delete;
    // 提交一次读取并尝试立即结算，有结果也不会内联调用消费者通知。
    std::shared_ptr<ReadRequest> next();
    // 由宿主在数据、关闭或取消变化后推进，取消请求先于本次取队列数据裁定。
    void advance();
    // 关闭交付队列后推进待完成读取，流结束与读取取消分别表达。
    void close(CloseMode mode) { queue_->close(mode); advance(); }
    // 回收没有客户端持有的终结请求，Pending 请求始终由宿主保持。
    void collect() noexcept;
    // 返回占用请求槽数，包含客户端仍持有的终结结果。
    std::size_t size() const noexcept { return size_; }
  private:
    // 保存类型化结果后结算唯一终态，并结束本控制器的 Pending 标记。
    void finish(ReadRequest& request, ReadSnapshot result, EventLease event = {});
    std::shared_ptr<SubscriptionQueue> queue_;
    std::vector<std::shared_ptr<ReadRequest>> requests_;
    std::optional<std::size_t> pending_;
    std::size_t size_ = 0;
};
}
