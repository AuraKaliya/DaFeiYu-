#pragma once
#include <dafeiyu/handle_registry.hpp>
#include <dafeiyu/operation.hpp>
#include <dafeiyu/fifo_executor.hpp>
#include <functional>

namespace dfy::runtime {
enum class NotificationState { pending, running, delivered, revoked };
enum class RevokeResult { revoked, already_inactive, busy };
enum class ScheduleResult { idle, queued, blocked };

// 同宿主线程的有界通知登记；客户端引用与待通知责任独立，捕获析构不得重入本池。
class CompletionNotifications {
  public:
    using Callback = std::function<void(const std::shared_ptr<const Operation>&)>;
    // 建立等容量的登记槽与不可共享引用表，拒绝零容量。
    explicit CompletionNotifications(std::size_t capacity);
    // 撤销未开始的通知，包括已排队项；已进入的调用自行收尾，不取消底层操作。
    ~CompletionNotifications();
    // 禁止复制登记责任与签发域。
    CompletionNotifications(const CompletionNotifications&) = delete;
    // 禁止赋值或隐式移动登记池。
    CompletionNotifications& operator=(const CompletionNotifications&) = delete;
    // 保持操作及回调并签发不可共享引用；失败不留下半登记，已完成操作也不内联通知。
    Handle subscribe(std::shared_ptr<const Operation> operation, Callback callback);
    // 返回独立登记状态副本，不驱动通知。
    NotificationState read(Handle registration) const { return references_.get(registration).record->state; }
    // 撤销尚未执行的登记，活动回调返回 busy；终结登记重复撤销无副作用。
    RevokeResult revoke(Handle registration);
    // 按登记顺序向 FIFO 提交一项已完成操作的通知；队满返回 blocked，消费者始终延后调用。
    ScheduleResult schedule_one(FifoExecutor& executor);
    // 按引用契约拒绝有效登记的共享，同时保留无效句柄的错误类别。
    Handle retain(Handle registration) { return references_.retain(registration); }
    // 释放客户端引用，不撤销 Pending 通知，再回收无持有者的终结记录。
    void release(Handle registration);
    // 回收无客户端或排队持有者的终结记录，不触碰 Pending／Running 登记。
    void collect() noexcept;
    // 返回占用登记槽数，包含仍被持有的终结记录。
    std::size_t size() const noexcept { return size_; }
    // 返回有效客户端引用数，不代表未交付通知数。
    std::size_t references() const noexcept { return references_.size(); }

  private:
    struct Record {
        std::shared_ptr<const Operation> operation;
        Callback callback;
        std::uint64_t order;
        NotificationState state = NotificationState::pending;
        bool queued = false;
    };
    struct Reference { std::shared_ptr<Record> record; };
    struct Scheduled {
        // 在 FIFO 包装中独立保持登记，不依赖登记池地址。
        explicit Scheduled(std::shared_ptr<Record> value) : record(std::move(value)) {}
        // 执行或丢弃包装后解除排队标记，使仍待交付的记录可以重新调度。
        ~Scheduled() { record->queued = false; }
        std::shared_ptr<Record> record;
    };
    // 释放消费者入口及操作持有后标记终结，不执行通知正文。
    static void finish(Record& record, NotificationState terminal) noexcept;
    // 在 FIFO 上交付一次通知；活动期覆盖回调及捕获释放，异常收尾后继续向执行器报告。
    static void deliver(const std::shared_ptr<Record>& record);
    std::vector<std::shared_ptr<Record>> records_;
    HandleRegistry<Reference> references_;
    std::size_t size_ = 0;
    std::uint64_t next_order_ = 1;
};
}
