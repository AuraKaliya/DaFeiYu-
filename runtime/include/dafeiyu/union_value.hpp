#pragma once
#include <dafeiyu/union_descriptor.hpp>

namespace dfy::runtime {
// 不可变联合值始终选择一个声明分支，内容为 null 时仍保留分支身份。
class UnionValue {
  public:
    // 按稳定标签选择分支，校验内容与标签共同占用的正文及节点预算。
    static UnionValue create(std::shared_ptr<const UnionDescriptor> descriptor, std::uint64_t tag, const Value& value,
        std::size_t max_payload_bytes, std::size_t max_nodes = Value::maximum_nodes);
    // 复制分支选择及内容，组合内容继续共享不可变快照。
    UnionValue(const UnionValue&) = default;
    // 禁止修改已经发布的分支或内容。
    UnionValue& operator=(const UnionValue&) = delete;
    // 返回独立描述持有，描述不保留联合数据。
    std::shared_ptr<const UnionDescriptor> descriptor() const noexcept { return descriptor_; }
    // 返回本描述内的选中索引，不代替稳定标签。
    std::size_t index() const noexcept { return index_; }
    // 返回选中分支的完整 UInt64 标签。
    std::uint64_t tag() const { return descriptor_->tag(index_); }
    // 借用选中分支内容，必须保持对应联合值持有。
    const Value& value() const noexcept { return value_; }
    // 返回 8 字节标签与实际分支正文的总量。
    std::size_t payload_bytes() const noexcept { return payload_bytes_; }
    // 返回联合根与实际分支内容的节点总数，null 内容也占节点。
    std::size_t node_count() const noexcept { return node_count_; }
  private:
    // 保存已校验的分支选择、不可变内容及聚合计量。
    UnionValue(std::shared_ptr<const UnionDescriptor> descriptor, std::size_t index, const Value& value,
        std::size_t bytes, std::size_t nodes)
        : descriptor_(std::move(descriptor)), index_(index), value_(value), payload_bytes_(bytes), node_count_(nodes) {}
    const std::shared_ptr<const UnionDescriptor> descriptor_;
    const std::size_t index_;
    const Value value_;
    const std::size_t payload_bytes_, node_count_;
};
}
