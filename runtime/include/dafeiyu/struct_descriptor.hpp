#pragma once
#include <dafeiyu/value.hpp>
#include <cstddef>

namespace dfy::runtime {
// 必填且不可空的基础字段；String/Bytes 使用 max_bytes，其他种类必须为零。
struct FieldDescriptor {
    std::string name;
    ValueKind kind;
    std::size_t max_bytes;
};

// 单份描述的字段数和所有名称正文的总字节上限，不代表全局内存预算。
struct DescriptorLimits {
    std::size_t max_fields;
    std::size_t max_name_bytes;
};

// 宿主持有的不可变结构描述，不包含 DLL 地址；名称与版本是契约身份，不是引用句柄。
class StructDescriptor {
  public:
    // 校验全部字段后复制描述，拒绝非法名称、零版本、重复字段、未知种类及容量超限。
    static StructDescriptor create(std::string_view name, std::uint32_t version,
        std::span<const FieldDescriptor> fields, DescriptorLimits limits);
    // 复制产生独立快照，字符串和字段均由新对象持有。
    StructDescriptor(const StructDescriptor&) = default;
    // 禁止赋值及隐式移动修改已发布快照。
    StructDescriptor& operator=(const StructDescriptor&) = delete;
    // 借用完整类型名，仅在对应描述或持有引用有效期间使用。
    std::string_view name() const noexcept { return name_; }
    // 返回精确契约版本，不表达 ABI 或插件版本。
    std::uint32_t version() const noexcept { return version_; }
    // 按声明顺序借用字段；索引只属于本描述，释放对应引用后须重新取得视图。
    std::span<const FieldDescriptor> fields() const noexcept { return fields_; }

  private:
    // 接收已校验的独立存储，固定快照内容。
    StructDescriptor(std::string name, std::uint32_t version, std::vector<FieldDescriptor> fields)
        : name_(std::move(name)), version_(version), fields_(std::move(fields)) {}
    const std::string name_;
    const std::uint32_t version_;
    const std::vector<FieldDescriptor> fields_;
};
}
