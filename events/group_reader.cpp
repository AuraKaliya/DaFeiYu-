#include <dafeiyu/group_reader.hpp>

namespace dfy::events {
// 先完成本地分配及参数校验，再取得成员登记，构造失败不会影响共享队列。
// 步骤：保存组与请求槽 → 校验非空和正容量 → 加入组。
GroupReader::GroupReader(std::shared_ptr<ConsumerGroup> group, std::size_t capacity)
    : group_(std::move(group)), requests_(capacity) {
    if (!group_ || !capacity) throw std::invalid_argument("group reader: group and positive capacity required");

    member_ = group_->join();
}
// 结束本控制器承担的等待和成员责任，不以析构取消整个组。
// 步骤：取消并结算 Pending → 撤销自身成员 → 由结果持有者继续管理已交付数据。
GroupReader::~GroupReader() {
    if (pending_) {
        auto& request = *requests_[*pending_];
        request.source_.request_cancel(runtime::CancelReason::owner_stop);
        finish(request,{ReadOutcome::cancelled,{}});
    }
    end(CloseMode::cancel);
}
// 撤销仍属于本控制器的登记，外部退出或槽位复用不导致误删新成员。
// 步骤：复用既有结束状态 → 按完整身份撤销成员 → 固定本地结束方式。
void GroupReader::end(CloseMode mode) {
    if (ended_) return;
    if (group_->has_member(member_)) group_->leave(member_);
    member_ = {};
    ended_ = mode;
}
// 只结束本成员接收，已经结束的组保留其原关闭方式。
// 步骤：确定结束方式并退出成员 → 推进本成员尚未结算的读取。
void GroupReader::close() {
    end(group_->state() == QueueState::closed ? *group_->close_mode() : CloseMode::cancel);
    advance();
}
// 在可用请求槽中登记完整等待，重复 Pending 或保留结果满额均明确拒绝。
// 步骤：回收终结项并检查重复 → 创建完整请求 → 登记宿主持有 → 尝试结算。
std::shared_ptr<ReadRequest> GroupReader::next() {
    collect();
    if (pending_) throw std::logic_error("group reader: read already pending");
    for (std::size_t index = 0; index < requests_.size(); ++index) {
        if (requests_[index]) continue;
        auto request = std::shared_ptr<ReadRequest>(new ReadRequest);

        requests_[index] = request; pending_ = index; ++size_;
        advance();
        return request;
    }
    throw std::length_error("group reader: request capacity exhausted");
}
// 固定结果与数据后发布操作终态，完成通知仍由宿主独立调度。
// 步骤：保存事件与结果 → 发布唯一终态 → 解除本控制器的等待标记。
void GroupReader::finish(ReadRequest& request, ReadSnapshot result, EventLease event) {
    request.event_ = std::move(event);
    request.result_ = result;

    request.source_.settle(result.outcome == ReadOutcome::cancelled ? runtime::OperationState::cancelled : runtime::OperationState::succeeded);
    pending_.reset();
}
// 按可见状态推进成员读取，取消或退出都不会领取留给其他成员的事件。
// 步骤：回收并观察成员／组结束 → 优先结算取消 → 按本地结束或组队列分流 → 结算并归还结束成员。
void GroupReader::advance() {
    collect();
    if (!ended_) {
        if (!group_->has_member(member_)) end(CloseMode::cancel);
        else if (group_->state() == QueueState::closed) end(*group_->close_mode());
    }
    if (!pending_) return;
    auto& request = *requests_[*pending_];
    if (request.operation()->cancellation_reason() != runtime::CancelReason::none) {
        finish(request,{ReadOutcome::cancelled,{}});
        return;
    }
    if (ended_) { finish(request,{ReadOutcome::closed,ended_}); return; }

    auto read = group_->poll(member_);
    if (read.state == PollState::empty) return;
    finish(request,{read.state == PollState::event ? ReadOutcome::event : ReadOutcome::closed,
        read.state == PollState::closed ? group_->close_mode() : std::nullopt},std::move(read.event));
    if (group_->state() == QueueState::closed) end(*group_->close_mode());
}
// 仅回收无客户端的终态，不能因客户端释放而丢弃未完成读取。
void GroupReader::collect() noexcept {
    for (auto& request : requests_) {
        if (!request || request->operation()->state() == runtime::OperationState::pending || request.use_count() != 1) continue;
        request.reset(); --size_;
    }
}
}
