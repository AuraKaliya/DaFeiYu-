#pragma once
#include <dafeiyu/handle_registry.hpp>
#include <dafeiyu/struct_value.hpp>

namespace dfy::events {
// 事件身份由接纳域及序号组成，独立于每份持有引用的域、槽位和代次。
struct EventId {
    runtime::DomainId domain = 0;
    std::uint64_t sequence = 0;
    // 比较完整事件身份，不比较持有引用或字段内容。
    bool operator==(const EventId&) const = default;
};
struct EventRecord {
    const EventId id;
    const std::shared_ptr<const runtime::StructValue> payload;
};

// 同一宿主线程内顺序接纳不可变事件，所有事件持有引用共用明确的容量。
class EventStore {
  public:
    // 建立独立接纳域、正引用容量和单事件正文上限，构造失败不返回存储。
    EventStore(std::size_t reference_capacity, std::size_t max_payload_bytes);
    // 释放表内引用；宿主须先结束使用该表的交付和消费者。
    ~EventStore() = default;
    // 禁止复制接纳身份和引用责任。
    EventStore(const EventStore&) = delete;
    // 禁止赋值或隐式移动正在被使用的事件存储。
    EventStore& operator=(const EventStore&) = delete;
    // 校验正文后接纳事件并返回持有引用，成功才推进序号；不表示消费者处理完成。
    runtime::Handle accept(std::shared_ptr<const runtime::StructValue> payload);
    // 原子签发同一新事件的多份引用，全部成功才推进接纳序号，失败回收本次全部持有。
    std::vector<runtime::Handle> accept_batch(std::shared_ptr<const runtime::StructValue> payload, std::size_t copies);
    // 取得同一事件的另一份持有引用，不产生新的接纳序号。
    runtime::Handle retain(runtime::Handle source) { return references_.retain(source); }
    // 释放一份事件引用，旧别名失效，不定义投递或业务完成。
    void release(runtime::Handle owned) { references_.release(owned); }
    // 在该持有引用有效期内借用事件及载荷，不延长借用本身的寿命。
    const EventRecord& get(runtime::Handle source) const { return references_.get(source); }
    // 返回当前持有引用数，不代表事件对象或投递数量。
    std::size_t references() const noexcept { return references_.size(); }
    // 返回剩余引用名额，供同宿主线程的交付接纳预检使用。
    std::size_t available_references() const noexcept { return references_.available(); }
    // 返回本存储独立的事件接纳域，不是引用表的签发域。
    runtime::DomainId domain() const noexcept { return domain_; }
  private:
    const runtime::DomainId domain_;
    runtime::HandleRegistry<const EventRecord> references_;
    const std::size_t max_payload_bytes_;
    std::uint64_t next_sequence_ = 1;
};
}
