#pragma once
#include <dafeiyu/subscription_queue.hpp>

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
    // 写入不可变消息并返回接纳身份，分类与版本取自载荷描述。
    EventId deliver(runtime::DomainId sender, MailboxId target, std::shared_ptr<const runtime::StructValue> payload);
    // 向原接收方转交队首数据，已读数据可以独立于信箱保留。
    QueueRead read(runtime::DomainId receiver, MailboxId target);
    // 撤销地址并放弃未读消息，已经交付的数据持有仍可读取。
    void close(runtime::DomainId receiver, MailboxId target);

  private:
    struct Entry {
        MailboxId id;
        runtime::DomainId receiver, sender;
        std::unique_ptr<SubscriptionQueue> queue;
    };
    // 查找当前目录中的有效地址，未知或已关闭的地址返回拒绝。
    std::vector<Entry>::iterator find(MailboxId target);
    const MailboxLimits limits_;
    std::shared_ptr<runtime::Budget> budget_;
    std::vector<Entry> entries_;
};
}
