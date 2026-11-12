#pragma once
#include <dafeiyu/struct_descriptor.hpp>
#include <memory>

namespace dfy::runtime {
// 构造期间借用的具名基础值；构造成功后不继续引用调用方容器。
struct NamedValue {
    std::string name;
    Value value;
};

// 宿主持有的不可变最小结构载荷，所有字段都是基础 Value，并独立保持描述寿命。
class StructValue {
  public:
    // 按描述校验字段及逻辑正文预算，复制并按描述顺序保存；不隐式转换类型。
    static StructValue create(std::shared_ptr<const StructDescriptor> descriptor,
        std::span<const NamedValue> fields, std::size_t max_payload_bytes);
    // 复制基础字段并共享不可变描述，不借用原载荷存储。
    StructValue(const StructValue&) = default;
    // 禁止赋值及隐式移动修改已有结构载荷。
    StructValue& operator=(const StructValue&) = delete;
    // 取得描述的独立共享持有，允许在结构载荷释放后继续读取元数据。
    std::shared_ptr<const StructDescriptor> descriptor() const noexcept { return descriptor_; }
    // 返回描述及载荷共有的字段数量。
    std::size_t size() const noexcept { return fields_.size(); }
    // 返回构造时核定的逻辑正文字节数，不包含描述、字段名及分配器开销。
    std::size_t payload_bytes() const noexcept { return payload_bytes_; }
    // 按描述内索引借用字段值，越界抛出 out_of_range；借用受结构载荷引用寿命约束。
    const Value& at(std::size_t index) const { return fields_.at(index); }
    // 按精确名称借用字段，未知名称返回空指针；不延长载荷寿命。
    const Value* find(std::string_view name) const noexcept;

  private:
    // 接收已校验的字段副本、描述持有及正文计量，固定结构载荷。
    StructValue(std::shared_ptr<const StructDescriptor> descriptor, std::vector<Value> fields, std::size_t payload_bytes)
        : descriptor_(std::move(descriptor)), fields_(std::move(fields)), payload_bytes_(payload_bytes) {}
    const std::shared_ptr<const StructDescriptor> descriptor_;
    const std::vector<Value> fields_;
    const std::size_t payload_bytes_;
};
}
