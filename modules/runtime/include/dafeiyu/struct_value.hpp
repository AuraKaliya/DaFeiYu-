#pragma once
#include <dafeiyu/struct_descriptor.hpp>
#include <memory>
#include <optional>

namespace dfy::runtime {
enum class FieldPresence { missing, null_value, value };
// 构造输入中的具名值；基础内容复制，组合内容共享不可变快照，不借用调用方容器。
struct NamedValue {
    std::string name;
    Value value;
};

// 宿主持有的不可变结构载荷，分别记录字段缺失、显式空值与实际内容。
class StructValue {
  public:
    // 按描述校验必填、可空、正文及节点预算并按声明顺序保存；不隐式转换或补默认值。
    static StructValue create(std::shared_ptr<const StructDescriptor> descriptor,
        std::span<const NamedValue> fields, std::size_t max_payload_bytes, std::size_t max_nodes = Value::maximum_nodes);
    // 复制字段槽位并共享不可变描述和组合内容，不借用原载荷的槽位存储。
    StructValue(const StructValue&) = default;
    // 禁止赋值及隐式移动修改已有结构载荷。
    StructValue& operator=(const StructValue&) = delete;
    // 取得描述的独立共享持有，允许在结构载荷释放后继续读取元数据。
    std::shared_ptr<const StructDescriptor> descriptor() const noexcept { return descriptor_; }
    // 返回描述及载荷共有的字段数量。
    std::size_t size() const noexcept { return fields_.size(); }
    // 返回构造时核定的逻辑正文字节数，不包含描述、字段名及分配器开销。
    std::size_t payload_bytes() const noexcept { return payload_bytes_; }
    // 返回包含根及所有实际字段内容的展开节点数，共享子值按出现次数计入。
    std::size_t node_count() const noexcept { return node_count_; }
    // 借用存在的字段，越界为 out_of_range，缺失为 bad_optional_access；显式 null 仍是值。
    const Value& at(std::size_t index) const { return fields_.at(index).value(); }
    // 返回字段是否存在，越界仍明确拒绝。
    bool present(std::size_t index) const { return fields_.at(index).has_value(); }
    // 区分缺失、显式 null 和实际值，不将空字符串视为空值。
    FieldPresence presence(std::size_t index) const {
        const auto& field = fields_.at(index);
        return !field ? FieldPresence::missing : field->kind() == ValueKind::null_value ? FieldPresence::null_value : FieldPresence::value;
    }
    // 按精确名称借用字段，未知名称或已声明但缺失返回空指针；可用 presence 区分。
    const Value* find(std::string_view name) const noexcept;

  private:
    // 接收已校验的字段副本、描述持有及正文计量，固定结构载荷。
    StructValue(std::shared_ptr<const StructDescriptor> descriptor, std::vector<std::optional<Value>> fields,
        std::size_t payload_bytes, std::size_t node_count)
        : descriptor_(std::move(descriptor)), fields_(std::move(fields)), payload_bytes_(payload_bytes), node_count_(node_count) {}
    const std::shared_ptr<const StructDescriptor> descriptor_;
    const std::vector<std::optional<Value>> fields_;
    const std::size_t payload_bytes_;
    const std::size_t node_count_;
};
}
