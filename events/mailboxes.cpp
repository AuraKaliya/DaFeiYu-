#include <dafeiyu/mailboxes.hpp>
#include <algorithm>
#include <limits>

namespace dfy::events {
// 将已知分类与有界 UTF-8 正文建立为同一不可变消息。
// 步骤：校验分类及正文上限 → 建立文本字段 → 固定完整载荷。
std::shared_ptr<const runtime::StructValue> mailbox_text(std::string_view name, std::uint32_t version,
    std::string_view text, std::size_t limit) {
    using namespace runtime;
    constexpr std::size_t name_limit = 128;
    if (name.empty() || name.size() > name_limit || !version) throw std::invalid_argument("mailboxes: invalid category");
    if (text.size() > limit) throw std::length_error("mailboxes: text exceeds message capacity");
    const FieldDescriptor fields[] = {{"text",ValueKind::string,limit}};
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create(name,version,fields,{1,name_limit+4}));
    const NamedValue contents[] = {{"text",Value::from_string(text,limit)}};
    return std::make_shared<const StructValue>(StructValue::create(std::move(descriptor),contents,limit));
}

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
MailboxId Mailboxes::open(runtime::DomainId receiver, runtime::DomainId sender, std::size_t payload_limit) {
    if (!receiver || !sender) throw std::invalid_argument("mailboxes: sender and receiver required");
    if (entries_.size() == limits_.mailboxes) throw std::length_error("mailboxes: directory full");

    auto store = std::make_shared<EventStore>(limits_.outstanding_events,payload_limit ? payload_limit : limits_.payload_bytes,budget_);
    const MailboxId id{store->domain()};
    entries_.push_back({id,receiver,sender,std::make_unique<SubscriptionQueue>(std::move(store),limits_.queue_depth)});
    return id;
}

// 仅接纳绑定发送者的下一个消息编号，重投归还本箱的原接纳身份。
// 步骤：校验发送方和消息 → 按水位分流重投或跳号 → 成功入队后提交水位及唤醒身份。
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

    const auto descriptor = payload->descriptor();
    const bool wakes = std::ranges::any_of(entry->wake_categories,[&](const auto& category) {
        return category.name == descriptor->name() && category.version == descriptor->version();
    });
    const auto event = entry->queue->accept(std::move(payload));
    entry->accepted = sequence;
    if (wakes) entry->wake = event;
    return {DeliveryState::accepted,event};
}

// 向绑定接收方转交首件，读到最新到件信号后清除唤醒。
// 步骤：检查接收权 → 转交队首 → 按读出身份清除已消费唤醒。
QueueRead Mailboxes::read(runtime::DomainId receiver, MailboxId target) {
    const auto entry = find(target);
    if (entry->receiver != receiver) throw std::invalid_argument("mailboxes: receiver not bound");
    auto result = entry->queue->poll();
    if (entry->wake && result.event && result.event.get().id == *entry->wake) entry->wake.reset();
    return result;
}

// 核对接收方后借用首件，准备失败时消息仍留在原队列。
const EventRecord* Mailboxes::front(runtime::DomainId receiver, MailboxId target) {
    const auto entry = find(target);
    if (entry->receiver != receiver) throw std::invalid_argument("mailboxes: receiver not bound");
    return entry->queue->front();
}

// 只允许原接收方查看送件身份。
runtime::DomainId Mailboxes::sender(runtime::DomainId receiver, MailboxId target) {
    const auto entry = find(target);
    if (entry->receiver != receiver) throw std::invalid_argument("mailboxes: receiver not bound");
    return entry->sender;
}

// 用接收方声明的精确分类替换后续投递的唤醒规则。
// 步骤：核对接收权及分类数 → 预检名称与版本 → 原子替换并清除旧规则信号。
void Mailboxes::wake_on(runtime::DomainId receiver, MailboxId target, std::span<const MailboxCategory> categories) {
    const auto entry = find(target);
    if (entry->receiver != receiver || categories.size() > 16)
        throw std::invalid_argument("mailboxes: invalid wake registration");
    for (const auto& category : categories)
        if (category.name.empty() || category.name.size() > 128 || !category.version)
            throw std::invalid_argument("mailboxes: invalid wake category");
    entry->wake_categories = std::vector<MailboxCategory>(categories.begin(),categories.end());
    entry->wake.reset();
}

// 查询轻量到件信号，当前读取量及消息队列保持不动。
std::optional<EventId> Mailboxes::pending_wake(runtime::DomainId receiver, MailboxId target) {
    const auto entry = find(target);
    if (entry->receiver != receiver) throw std::invalid_argument("mailboxes: receiver not bound");
    return entry->wake;
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
