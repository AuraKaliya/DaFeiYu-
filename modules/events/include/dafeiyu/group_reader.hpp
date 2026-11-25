#pragma once
#include <dafeiyu/consumer_group.hpp>
#include <dafeiyu/subscription_reader.hpp>

namespace dfy::events {
// 同宿主线程的组成员读取控制器，独占一个成员登记，不关闭其他成员的共享队列。
class GroupReader {
  public:
    // 建立正请求容量后加入开放组，构造失败不留下成员登记。
    GroupReader(std::shared_ptr<ConsumerGroup> group, std::size_t request_capacity);
    // 取消尚未结算的等待并退出自身成员，已经交付的数据继续独立存活。
    ~GroupReader();
    // 禁止复制成员身份和读取完成权。
    GroupReader(const GroupReader&) = delete;
    // 禁止赋值或隐式移动正在使用的控制器。
    GroupReader& operator=(const GroupReader&) = delete;
    // 提交至多一项 Pending，保留结果也占请求容量；有事件时可立即结算但不回调消费者。
    std::shared_ptr<ReadRequest> next();
    // 显式推进成员退出、取消、领取及组结束，取消优先于本次领取。
    void advance();
    // 退出本成员并结束等待，不关闭组或重放已领取数据；重复关闭无副作用。
    void close();
    // 回收没有客户端持有的终结结果，Pending 始终由控制器保留。
    void collect() noexcept;
    // 返回尚未撤销的成员身份，控制器观察到退出后为空。
    MemberId member() const noexcept { return member_; }
    // 返回占用请求槽数，包括客户端仍持有的终结结果。
    std::size_t size() const noexcept { return size_; }
    // 返回本成员观察到的接收阶段，查询本身不推进退出或读取。
    QueueState queue_state() const noexcept { return ended_ ? QueueState::closed : group_->state(); }
    // 返回成员退出或组结束模式，已确定的本地模式不再改写。
    std::optional<CloseMode> close_mode() const noexcept { return ended_ ? ended_ : group_->close_mode(); }
    // 返回可供当前成员竞争的组队列长度，成员退出后为零。
    std::size_t queued() const noexcept { return ended_ ? 0 : group_->queued(); }
  private:
    // 撤销仍有效的自身成员并固定本地结束模式，不影响已经复用该槽位的新成员。
    void end(CloseMode mode);
    // 保存类型化结果后发布唯一终态，并结束本控制器的 Pending 标记。
    void finish(ReadRequest& request, ReadSnapshot result, EventLease event = {});
    std::shared_ptr<ConsumerGroup> group_;
    std::vector<std::shared_ptr<ReadRequest>> requests_;
    MemberId member_;
    std::optional<CloseMode> ended_;
    std::optional<std::size_t> pending_;
    std::size_t size_ = 0;
};
}
