#pragma once
#include <dafeiyu/broadcast_channel.hpp>
#include <dafeiyu/processing.hpp>
#include <dafeiyu/result_group.hpp>
#include <chrono>

namespace dfy::events {
enum class ObservationStatus { accepted, overloaded, invalid, failed };
struct ObservationReceipt { ObservationStatus status; EventId event; };
// 接纳普通观察通知，失败返回独立回执而不传播异常；Accepted 不表示观察处理已成功。
ObservationReceipt observe(BroadcastChannel& channel, std::shared_ptr<const runtime::StructValue> payload) noexcept;
enum class ControlDecision { pending, allowed, denied, cancelled, timed_out };

// 显式控制边界，要求全部参与者成功才允许继续；宿主仍须另行等待实际退出。
class ControlGate {
  public:
    using TimePoint = std::chrono::steady_clock::time_point;
    // 固定互异非空处理记录、截止时刻和可选结果预算；空控制集合由调用方显式选择跳过。
    ControlGate(std::vector<std::shared_ptr<ProcessingRecord>> participants, TimePoint deadline,
        std::shared_ptr<runtime::Budget> budget = {});
    // 禁止复制控制决策及取消责任。
    ControlGate(const ControlGate&) = delete;
    // 禁止赋值或隐式移动控制边界。
    ControlGate& operator=(const ControlGate&) = delete;
    // 在单调时刻裁定截止和已报告结果，达到截止时先拒绝尚未确定的决策，不调用消费者。
    void advance(TimePoint now);
    // 请求取消尚未确定的控制决策，并向未报告参与者传播首个原因。
    runtime::CancelDisposition cancel(runtime::CancelReason reason);
    // 返回当前类型化决策，只有 allowed 可以继续受控业务。
    ControlDecision decision() const noexcept;
    // 取得逻辑决策操作供完成通知等待，不把它作为资源退出证明。
    std::shared_ptr<const runtime::Operation> operation() const noexcept { return group_.result()->operation(); }
    // 判断所有参与者已实际退出，逻辑否决或超时不使该条件提前成立。
    bool exited() const noexcept;
  private:
    // 将显式取消目标传播到处理记录，不重写已报告业务结果。
    void propagate(runtime::CancelReason reason);
    std::vector<std::shared_ptr<ProcessingRecord>> participants_;
    runtime::ResultGroup group_;
    const TimePoint deadline_;
    std::optional<TimePoint> last_;
    bool timed_out_ = false;
};
}
