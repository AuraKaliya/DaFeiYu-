#include <dafeiyu/subscription_reader.hpp>

namespace dfy::events {
// 返回确定的读取结果，Pending 不伪装成流结束。
ReadSnapshot ReadRequest::result() const {
    if (!result_) throw std::logic_error("read: result pending");
    return *result_;
}

// 在结果确实为事件时借用记录，不从取消或关闭结果制造空事件。
const EventRecord& ReadRequest::event() const {
    if (result().outcome != ReadOutcome::event) throw std::logic_error("read: no event result");
    return event_.get();
}

// 显式延长事件数据寿命，读取请求仍保持自己的原始引用。
EventLease ReadRequest::retain_event() const { (void)event(); return event_.retain(); }

// 建立固定请求槽，队列及请求预算都由调用方明确提供。
SubscriptionReader::SubscriptionReader(std::shared_ptr<SubscriptionQueue> queue, std::size_t request_capacity)
    : queue_(std::move(queue)), requests_(request_capacity) {
    if (!queue_ || !request_capacity) throw std::invalid_argument("reader: queue and positive capacity required");
}

// 结束控制器仍承担的读取，不把控制器退出当作整个订阅已关闭。
// 步骤：检查 Pending → 登记所有者停止原因 → 结算本次读取取消。
SubscriptionReader::~SubscriptionReader() {
    if (!pending_) return;
    auto& request = *requests_[*pending_];
    request.source_.request_cancel(runtime::CancelReason::owner_stop);
    finish(request, {ReadOutcome::cancelled, {}});
}

// 在预算内提交单项读取，客户端释放请求不会丢弃 Pending 责任。
// 步骤：回收终结项并拒绝重复等待 → 创建请求 → 登记宿主持有 → 尝试结算。
std::shared_ptr<ReadRequest> SubscriptionReader::next() {
    collect();
    if (pending_) throw std::logic_error("reader: read already pending");
    for (std::size_t index = 0; index < requests_.size(); ++index) {
        if (requests_[index]) continue;
        auto request = std::shared_ptr<ReadRequest>(new ReadRequest);
        requests_[index] = request;
        pending_ = index;
        ++size_;
        advance();
        return request;
    }
    throw std::length_error("reader: request capacity exhausted");
}

// 将类型化结果固定到请求，再发布操作终态；完成通知仍由独立通知服务调度。
void SubscriptionReader::finish(ReadRequest& request, ReadSnapshot result, EventLease event) {
    request.event_ = std::move(event);
    request.result_ = result;
    request.source_.settle(result.outcome == ReadOutcome::cancelled ? runtime::OperationState::cancelled : runtime::OperationState::succeeded);
    pending_.reset();
}

// 根据可见取消、队列数据或结束状态推进读取，不阻塞或回调消费者。
// 步骤：回收并定位 Pending → 优先处理已请求取消 → 尝试取事件 → 按事件或流结束结算。
void SubscriptionReader::advance() {
    collect();
    if (!pending_) return;
    auto& request = *requests_[*pending_];
    if (request.operation()->cancellation_reason() != runtime::CancelReason::none) {
        finish(request, {ReadOutcome::cancelled, {}});
        return;
    }
    auto read = queue_->poll();
    if (read.state == PollState::empty) return;
    finish(request, {read.state == PollState::event ? ReadOutcome::event : ReadOutcome::closed,
        read.state == PollState::closed ? queue_->close_mode() : std::nullopt},
        std::move(read.event));
}

// 只回收无客户端持有的终结请求，不丢失尚未完成的工作或借用数据。
void SubscriptionReader::collect() noexcept {
    for (auto& request : requests_) {
        if (!request || request->operation()->state() == runtime::OperationState::pending || request.use_count() != 1) continue;
        request.reset();
        --size_;
    }
}
}
