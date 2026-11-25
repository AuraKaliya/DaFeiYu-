#pragma once
#include <dafeiyu/group_reader.hpp>
#include <variant>
#include <type_traits>

namespace dfy::plugins::detail {
// 宿主内部的读取转发持有，统一数据读取，保留独立订阅和组成员的关闭边界。
class ReaderReference {
  public:
    // 保存独立订阅读取控制器，不复制其完成权。
    explicit ReaderReference(std::shared_ptr<events::SubscriptionReader> reader) : source_(std::move(reader)) {}
    // 保存组成员读取控制器，不取得整个组的关闭权。
    explicit ReaderReference(std::shared_ptr<events::GroupReader> reader) : source_(std::move(reader)) {}
    // 释放转发持有，实际控制器由其最后持有者回收。
    ~ReaderReference() = default;
    // 禁止复制转发对象，边界引用共享同一转发持有。
    ReaderReference(const ReaderReference&) = delete;
    // 禁止替换已发布的控制器种类或对象。
    ReaderReference& operator=(const ReaderReference&) = delete;
    // 提交对应控制器的一次类型化读取。
    std::shared_ptr<events::ReadRequest> next() { return std::visit([](const auto& reader) { return reader->next(); },source_); }
    // 显式推进对应控制器，不在转发层调用消费者代码。
    void advance() { std::visit([](const auto& reader) { reader->advance(); },source_); }
    // 回收对应控制器已无客户端的终结请求。
    void collect() noexcept { std::visit([](const auto& reader) { reader->collect(); },source_); }
    // 返回当前接收方状态，组成员退出与共享组状态分别由控制器表达。
    events::QueueState queue_state() const noexcept { return std::visit([](const auto& reader) { return reader->queue_state(); },source_); }
    // 比较实际控制器身份，防止同一宿主重复登记同一读取责任。
    bool same_source(const ReaderReference& other) const noexcept { return source_ == other.source_; }
    // 只借用独立订阅控制器；组成员调用会因种类不符而拒绝。
    std::shared_ptr<events::SubscriptionReader> subscription() const { return std::get<std::shared_ptr<events::SubscriptionReader>>(source_); }
    // 只借用组成员控制器，不将独立订阅当作成员流。
    std::shared_ptr<events::GroupReader> group_member() const { return std::get<std::shared_ptr<events::GroupReader>>(source_); }
    // 结束本接收方；组成员只退出自身，独立订阅取消关闭自己的队列。
    void cancel() { std::visit([](const auto& reader) {
        if constexpr (std::is_same_v<typename std::decay_t<decltype(reader)>::element_type,events::GroupReader>) reader->close();
        else reader->close(events::CloseMode::cancel);
    },source_); }
  private:
    const std::variant<std::shared_ptr<events::SubscriptionReader>,std::shared_ptr<events::GroupReader>> source_;
};
}
