#pragma once
#include <dafeiyu/array_value.hpp>

namespace dfy::runtime {
// 映射以 UTF-8 字符串为键，值持有完整匿名约束并可独立允许 null。
class MapDescriptor {
  public:
    // 校验条目、键和标量值约束；数量上限沿用 65536 的容器硬边界。
    static MapDescriptor create(ValueKind value_kind, std::size_t max_entries, std::size_t max_key_bytes,
        std::size_t max_value_bytes = 0, bool nullable = false);
    // 使用完整匿名值约束创建映射，条目数量仍受 65536 的硬上限限制。
    static MapDescriptor create(const ElementDescriptor& element, std::size_t max_entries, std::size_t max_key_bytes);
    // 复制不可变映射约束。
    MapDescriptor(const MapDescriptor&) = default;
    // 禁止修改已经发布的约束。
    MapDescriptor& operator=(const MapDescriptor&) = delete;
    // 返回声明的非空值种类。
    ValueKind value_kind() const noexcept { return values_.element_kind(); }
    // 返回条目数量上限，零只允许空映射。
    std::size_t max_entries() const noexcept { return values_.max_elements(); }
    // 返回单个键的 UTF-8 字节上限，零允许空键。
    std::size_t max_key_bytes() const noexcept { return max_key_bytes_; }
    // 返回单个 String/Bytes 值的正文上限。
    std::size_t max_value_bytes() const noexcept { return values_.max_element_bytes(); }
    // 判断条目值是否可以是显式 null。
    bool nullable() const noexcept { return values_.nullable(); }
    // 借用当前映射的匿名值约束，键约束仍由映射独立保存。
    const ElementDescriptor& element() const noexcept { return values_.element(); }
    // 返回包含映射层及值内容的描述深度。
    std::size_t depth() const noexcept { return values_.depth(); }
    // 返回匿名值条目及其子描述的展开数量。
    std::size_t total_fields() const noexcept { return values_.total_fields(); }
    // 返回值约束中命名子描述的展开名称正文。
    std::size_t name_bytes() const noexcept { return values_.name_bytes(); }
    // 比较完整映射约束，键和值预算都是定义的一部分。
    bool equivalent(const MapDescriptor& other) const noexcept { return values_.equivalent(other.values_) && max_key_bytes_ == other.max_key_bytes_; }
  private:
    // 接收已经校验的完整内容约束和键上限。
    MapDescriptor(ArrayDescriptor values, std::size_t key_bytes) : values_(std::move(values)), max_key_bytes_(key_bytes) {}
    const ArrayDescriptor values_;
    const std::size_t max_key_bytes_;
};
struct MapEntry { std::string key; Value value; };
// 不可变映射按 UTF-8 字节排序，键属于数据正文，不进行大小写或 Unicode 规范化。
class MapValue {
  public:
    // 校验键、唯一性、值类型及两层预算后复制并排序，不修改输入。
    static MapValue create(std::shared_ptr<const MapDescriptor> descriptor, std::span<const MapEntry> entries,
        std::size_t max_payload_bytes, std::size_t max_nodes = Value::maximum_nodes);
    // 复制条目并共享不可变约束。
    MapValue(const MapValue&) = default;
    // 禁止修改已经发布的映射。
    MapValue& operator=(const MapValue&) = delete;
    // 取得约束的独立持有。
    std::shared_ptr<const MapDescriptor> descriptor() const noexcept { return descriptor_; }
    // 返回实际条目数量。
    std::size_t size() const noexcept { return entries_.size(); }
    // 按确定字节序借用条目，越界抛出 out_of_range。
    const MapEntry& at(std::size_t index) const { return entries_.at(index); }
    // 按完整键查询值，未知键返回空指针，存在的 null 返回真实值。
    const Value* find(std::string_view key) const noexcept;
    // 返回包含键和实际值正文的逻辑字节数。
    std::size_t payload_bytes() const noexcept { return payload_bytes_; }
    // 根与每个键各占节点，实际值按完整节点累计，空键和 null 也计入。
    std::size_t node_count() const noexcept { return node_count_; }
  private:
    // 保存已校验的排序副本及正文计量。
    MapValue(std::shared_ptr<const MapDescriptor> descriptor, std::vector<MapEntry> entries, std::size_t bytes, std::size_t nodes)
        : descriptor_(std::move(descriptor)), entries_(std::move(entries)), payload_bytes_(bytes), node_count_(nodes) {}
    const std::shared_ptr<const MapDescriptor> descriptor_;
    const std::vector<MapEntry> entries_;
    const std::size_t payload_bytes_, node_count_;
};
}
