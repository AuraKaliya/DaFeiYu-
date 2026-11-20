#pragma once
#include <dafeiyu/subscription_queue.hpp>
#include <span>

namespace dfy::events {
struct MailboxId {
    runtime::DomainId value = 0;
    // 比较信箱地址，关闭后重开会取得不同地址。
    bool operator==(const MailboxId&) const = default;
};
struct MailboxLimits {
    std::size_t mailboxes;
    std::size_t queue_depth;
    std::size_t outstanding_events;
    std::size_t payload_bytes;
    std::size_t receipt_window = 64;
};
enum class DeliveryState { accepted, repeated, expired, out_of_order };
struct DeliveryReceipt {
    DeliveryState state;
    EventId event;
};
struct MailboxCategory {
    std::string name;
    std::uint32_t version;
};

// 在宿主线程上管理定向信箱。读取方和发送方必须来自宿主绑定的身份。
class Mailboxes {
  public:
    // 建立目录容量、各箱队列容量和全局未释放消息预算。
    explicit Mailboxes(MailboxLimits limits);
    // 禁止复制队列与接纳责任。
    Mailboxes(const Mailboxes&) = delete;
    // 禁止赋值或隐式移动信箱目录。
    Mailboxes& operator=(const Mailboxes&) = delete;
    // 绑定接收方与允许投递的一方，签发进程期不复用的信箱地址。
    MailboxId open(runtime::DomainId receiver, runtime::DomainId sender);
    // 发送序号在目标信箱内从 1 连续递增；窗口内重投只读取原回执，消息正文仍取自首次接纳。
    DeliveryReceipt deliver(runtime::DomainId sender, MailboxId target, std::uint64_t sequence,
        std::shared_ptr<const runtime::StructValue> payload);
    // 向原接收方转交队首数据，已读数据可以独立于信箱保留。
    QueueRead read(runtime::DomainId receiver, MailboxId target);
    // 在同宿主线程借用当前队首，用于准备读取方的持有引用。
    const EventRecord* front(runtime::DomainId receiver, MailboxId target);
    // 由接收方配置新消息的唤醒分类，空列表恢复主动收件。
    void wake_on(runtime::DomainId receiver, MailboxId target, std::span<const MailboxCategory> categories);
    // 查询尚未读取的最晚唤醒身份，读取已到该身份后自动清除。
    std::optional<EventId> pending_wake(runtime::DomainId receiver, MailboxId target);
    // 撤销地址并放弃未读消息，已经交付的数据持有仍可读取。
    void close(runtime::DomainId receiver, MailboxId target);
    // 撤销已退出一方参与的所有信箱，地址与未读消息同时撤销。
    void revoke(runtime::DomainId participant) noexcept;
    // 读取单条消息正文上限，发送方可在复制前校验。
    std::size_t payload_limit() const noexcept { return limits_.payload_bytes; }

  private:
    struct Entry {
        MailboxId id;
        runtime::DomainId receiver, sender;
        std::unique_ptr<SubscriptionQueue> queue;
        std::uint64_t accepted = 0;
        std::vector<MailboxCategory> wake_categories{};
        std::optional<EventId> wake{};
    };
    // 查找当前目录中的有效地址，未知或已关闭的地址返回拒绝。
    std::vector<Entry>::iterator find(MailboxId target);
    const MailboxLimits limits_;
    std::shared_ptr<runtime::Budget> budget_;
    std::vector<Entry> entries_;
};
}
