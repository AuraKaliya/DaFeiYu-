#pragma once
#include <dafeiyu/subscription_queue.hpp>
#include <dafeiyu/serial_domain.hpp>

namespace dfy::events {
enum class ProcessingPhase { waiting, running, exited };
enum class StartResult { waiting, started, exited };
class ProcessingSource;

// 真实事件的一次处理记录；业务结果、实际退出和数据持有分别独立。
class ProcessingRecord {
  public:
    // 禁止复制处理身份及其完成权。
    ProcessingRecord(const ProcessingRecord&) = delete;
    // 禁止赋值或隐式移动处理记录。
    ProcessingRecord& operator=(const ProcessingRecord&) = delete;
    // 返回处理阶段，完成业务结果不会提前进入 exited。
    ProcessingPhase phase() const noexcept { return phase_; }
    // 返回唯一业务终态；该视图不承担协作取消，取消通过本记录请求。
    std::shared_ptr<const runtime::Operation> completion() const noexcept { return completion_.operation(); }
    // 返回实际退出确认，Succeeded 只表示处理不再运行，不表达业务成功。
    std::shared_ptr<const runtime::Operation> exit() const noexcept { return exit_.operation(); }
    // 请求取消处理，已开始时仅记录意图，宿主必须等待实际退出。
    runtime::CancelDisposition request_cancel(runtime::CancelReason reason) { return exit_.request_cancel(reason); }
    // 借用不可变事件，借用期由本记录或独立事件持有保证。
    const EventRecord& event() const { return event_.get(); }
    // 独立保留事件数据，不延长业务占用或阻止处理退出。
    EventLease retain_event() const { return event_.retain(); }
    // 返回接受处理时固定的顺序请求，无顺序约束时为空。
    std::shared_ptr<const runtime::SerialRequest> ordering() const noexcept { return ordering_; }
  private:
    friend class ProcessingSource;
    // 完整准备数据和结果记录，尚不绑定或开始处理。
    ProcessingRecord(const EventLease& event, std::shared_ptr<const runtime::SerialRequest> ordering)
        : event_(event.retain()), ordering_(std::move(ordering)) {}
    EventLease event_;
    const std::shared_ptr<const runtime::SerialRequest> ordering_;
    ProcessingPhase phase_ = ProcessingPhase::waiting;
    runtime::OperationSource completion_;
    runtime::OperationSource exit_{true};
};

// 同宿主线程的唯一处理完成源，宿主必须保留至 finish；客户端释放不会结束处理。
class ProcessingSource {
  public:
    // 先保留真实事件及准备结果，再独占绑定已有顺序请求；无顺序约束时两个参数均为空。
    ProcessingSource(const EventLease& event, std::shared_ptr<runtime::SerialDomain> domain = {},
        std::shared_ptr<const runtime::SerialRequest> ordering = {});
    // 取消尚未开始处理；运行中销毁属于宿主违约，不伪造结果、退出或释放占用。
    ~ProcessingSource();
    // 禁止复制处理完成权。
    ProcessingSource(const ProcessingSource&) = delete;
    // 禁止赋值或隐式移动处理完成源。
    ProcessingSource& operator=(const ProcessingSource&) = delete;
    // 返回独立持有的处理记录，不转交宿主完成权。
    std::shared_ptr<ProcessingRecord> record() const noexcept { return record_; }
    // 裁定开始前取消及顺序资格；不推进顺序域、不执行消费者或通知。
    StartResult start();
    // 固定运行中的唯一业务结果，仍保留顺序占用直至实际退出。
    void complete(runtime::OperationState terminal);
    // 在业务已有结果且处理真正退出后归还顺序占用，独立数据持有继续有效。
    void finish();
  private:
    // 撤回未开始处理，取消业务结果并确认无需等待运行退出。
    void cancel_unstarted();
    const std::shared_ptr<runtime::SerialDomain> domain_;
    const std::shared_ptr<ProcessingRecord> record_;
};
}
