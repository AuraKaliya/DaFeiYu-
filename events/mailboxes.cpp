#include <dafeiyu/mailboxes.hpp>
#include <algorithm>
#include <limits>

namespace dfy::events {
// 为全部信箱建立共享消息预算，关闭后仍由读出事件持有的消息继续计费。
// 步骤：校验信箱与消息容量 → 建立共享预算 → 预留目录槽位。
Mailboxes::Mailboxes(MailboxLimits limits) : limits_(limits) {
    if (!limits.mailboxes || !limits.queue_depth || !limits.outstanding_events || !limits.receipt_window)
        throw std::invalid_argument("mailboxes: positive capacities required");

    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    const auto bytes = limits.payload_bytes > maximum / limits.outstanding_events
        ? maximum : limits.payload_bytes * limits.outstanding_events;
    budget_ = std::make_shared<runtime::Budget>(runtime::BudgetAmount{limits.outstanding_events,bytes});
    entries_.reserve(limits.mailboxes);
}

// 在有界目录中建立与原发送者和接收者绑定的独立队列。
// 步骤：检查双方身份及槽位 → 为独立序号准备事件存储 → 提交队列与地址。
MailboxId Mailboxes::open(runtime::DomainId receiver, runtime::DomainId sender) {
    if (!receiver || !sender) throw std::invalid_argument("mailboxes: sender and receiver required");
    if (entries_.size() == limits_.mailboxes) throw std::length_error("mailboxes: directory full");

    auto store = std::make_shared<EventStore>(limits_.outstanding_events,limits_.payload_bytes,budget_);
    const MailboxId id{store->domain()};
    entries_.push_back({id,receiver,sender,std::make_unique<SubscriptionQueue>(std::move(store),limits_.queue_depth)});
    return id;
}

// 仅接纳绑定发送者的下一个消息编号，重投归还本箱的原接纳身份。
// 步骤：校验发送方和消息 → 按水位分流重投或跳号 → 成功入队后提交水位。
DeliveryReceipt Mailboxes::deliver(runtime::DomainId sender, MailboxId target, std::uint64_t sequence,
    std::shared_ptr<const runtime::StructValue> payload) {
    const auto entry = find(target);
    if (entry->sender != sender) throw std::invalid_argument("mailboxes: sender not bound");
    if (!sequence || !payload) throw std::invalid_argument("mailboxes: sequence and payload required");

    if (sequence <= entry->accepted) {
        if (entry->accepted - sequence >= limits_.receipt_window) return {DeliveryState::expired,{}};
        return {DeliveryState::repeated,{target.value,sequence}};
    }
    if (sequence - entry->accepted != 1) return {DeliveryState::out_of_order,{}};

    const auto event = entry->queue->accept(std::move(payload));
    entry->accepted = sequence;
    return {DeliveryState::accepted,event};
}

// 仅向绑定的接收方转交队首数据。
QueueRead Mailboxes::read(runtime::DomainId receiver, MailboxId target) {
    const auto entry = find(target);
    if (entry->receiver != receiver) throw std::invalid_argument("mailboxes: receiver not bound");
    return entry->queue->poll();
}

// 核对接收方后借用首件，准备失败时消息仍留在原队列。
const EventRecord* Mailboxes::front(runtime::DomainId receiver, MailboxId target) {
    const auto entry = find(target);
    if (entry->receiver != receiver) throw std::invalid_argument("mailboxes: receiver not bound");
    return entry->queue->front();
}

// 撤销本方的信箱地址并释放队列持有。
void Mailboxes::close(runtime::DomainId receiver, MailboxId target) {
    const auto entry = find(target);
    if (entry->receiver != receiver) throw std::invalid_argument("mailboxes: receiver not bound");
    entries_.erase(entry);
}

// 同时撤销旧实例的发送及接收地址，已读数据由原持有者管理。
void Mailboxes::revoke(runtime::DomainId participant) noexcept {
    std::erase_if(entries_,[=](const auto& entry) { return entry.receiver == participant || entry.sender == participant; });
}

// 在当前目录中查找非过期地址。
std::vector<Mailboxes::Entry>::iterator Mailboxes::find(MailboxId target) {
    const auto entry = std::ranges::find(entries_,target,&Entry::id);
    if (entry == entries_.end()) throw std::invalid_argument("mailboxes: address unavailable");
    return entry;
}
}
