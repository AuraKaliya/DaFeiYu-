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
struct MailboxBinding {
    MailboxId mailbox;
    runtime::DomainId sender, receiver;
};
// 仅保留投递与实际取件的公开元数据，不持有载荷及事件引用。
struct MailboxMessageReceipt {
    EventId event;
    std::string category;
    std::uint32_t version = 0;
    std::int64_t accepted_utc_ms = 0;
    std::optional<std::int64_t> received_utc_ms;
    std::uint64_t accepted_revision = 0, received_revision = 0;
};
struct MailboxInspection {
    std::uint64_t revision, accepted, received;
    std::vector<MailboxMessageReceipt> messages;
};

// 创建由宿主拥有的单文本分类载荷，适用于原生服务及本体发送入口。
std::shared_ptr<const runtime::StructValue> mailbox_text(std::string_view category, std::uint32_t version,
    std::string_view text, std::size_t limit);

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
    MailboxId open(runtime::DomainId receiver, runtime::DomainId sender, std::size_t payload_limit = 0);
    // 返回实际开箱、关闭或撤销后的目录修订，查看及重复撤销不改变它。
    std::uint64_t directory_revision() const noexcept { return directory_revision_; }
    // 复制指定参与方当前绑定的地址及双方身份，保留系统的消费位置。
    std::vector<MailboxBinding> bindings(runtime::DomainId participant) const;
    // 返回本网络最近的地址变动、成功投递或实际取件修订。
    std::uint64_t revision() const noexcept { return revision_; }
    // 借原参与方身份读取该地址的最近修订，用于跳过未变的回执。
    std::uint64_t revision(runtime::DomainId participant, MailboxId target) const;
    // 复制原地址的有界公开回执及真实读写水位，查询不会更新收件或唤醒。
    MailboxInspection inspect(runtime::DomainId participant, MailboxId target) const;
    // 发送序号在目标信箱内从 1 连续递增；窗口内重投只读取原回执，消息正文仍取自首次接纳。
    DeliveryReceipt deliver(runtime::DomainId sender, MailboxId target, std::uint64_t sequence,
        std::shared_ptr<const runtime::StructValue> payload);
    // 由独占本地发送方接纳下一顺序号，拒收时不推进水位。
    DeliveryReceipt deliver_next(runtime::DomainId sender, MailboxId target, std::shared_ptr<const runtime::StructValue> payload);
    // 向原接收方转交队首数据，已读数据可以独立于信箱保留。
    QueueRead read(runtime::DomainId receiver, MailboxId target);
    // 在同宿主线程借用当前队首，用于准备读取方的持有引用。
    const EventRecord* front(runtime::DomainId receiver, MailboxId target);
    // 返回接收方当前信箱绑定的可信发送者。
    runtime::DomainId sender(runtime::DomainId receiver, MailboxId target);
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
        std::vector<MailboxMessageReceipt> receipts{};
        std::uint64_t revision = 0;
    };
    // 查找当前目录中的有效地址，未知或已关闭的地址返回拒绝。
    std::vector<Entry>::iterator find(MailboxId target);
    // 仅向原信箱实际绑定的一方提供同线程的只读记录。
    const Entry& bound(runtime::DomainId participant, MailboxId target) const;
    const MailboxLimits limits_;
    std::shared_ptr<runtime::Budget> budget_;
    std::vector<Entry> entries_;
    std::uint64_t directory_revision_ = 0;
    std::uint64_t revision_ = 0;
};
}
