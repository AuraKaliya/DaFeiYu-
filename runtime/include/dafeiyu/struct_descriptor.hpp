#pragma once
#include <dafeiyu/value.hpp>
#include <cstddef>

namespace dfy::runtime {
class StructDescriptor;
class ArrayDescriptor;
// 字段的可选与可空独立表达；结构必须提供描述，其他种类不能携带结构描述。
struct FieldDescriptor {
    std::string name;
    ValueKind kind;
    std::size_t max_bytes;
    std::shared_ptr<const StructDescriptor> structure{};
    bool optional = false;
    bool nullable = false;
    std::shared_ptr<const ArrayDescriptor> array{};
    // 构造数组字段约束，仍独立表达字段本身的可选与可空。
    static FieldDescriptor array_field(std::string name, std::shared_ptr<const ArrayDescriptor> descriptor,
        bool optional = false, bool nullable = false) {
        return {std::move(name), ValueKind::array, 0, {}, optional, nullable, std::move(descriptor)};
    }
};

// 单份描述的字段数和所有名称正文的总字节上限，不代表全局内存预算。
struct DescriptorLimits {
    std::size_t max_fields;
    std::size_t max_name_bytes;
    std::size_t max_depth = 1;
};

// 宿主持有的不可变结构描述，不包含 DLL 地址；名称与版本是契约身份，不是引用句柄。
class StructDescriptor {
  public:
    static constexpr std::size_t maximum_depth = 64;
    static constexpr std::size_t maximum_fields = 65536;
    static constexpr std::size_t maximum_name_bytes = 4 * 1024 * 1024;
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
    // 返回包含本结构的嵌套深度，最小为一。
    std::size_t depth() const noexcept { return depth_; }
    // 返回按字段出现次数展开的总字段数，共享描述也逐次计入。
    std::size_t total_fields() const noexcept { return total_fields_; }
    // 返回按字段出现次数展开的名称正文总量，不代表实际分配字节。
    std::size_t name_bytes() const noexcept { return name_bytes_; }
    // 比较完整身份、字段顺序及嵌套约束，不以指针或名称相同代替定义相同。
    bool equivalent(const StructDescriptor& other) const noexcept;

  private:
    // 接收已校验的独立存储，固定快照内容。
    StructDescriptor(std::string name, std::uint32_t version, std::vector<FieldDescriptor> fields,
        std::size_t depth, std::size_t total_fields, std::size_t name_bytes)
        : name_(std::move(name)), version_(version), fields_(std::move(fields)), depth_(depth), total_fields_(total_fields), name_bytes_(name_bytes) {}
    const std::string name_;
    const std::uint32_t version_;
    const std::vector<FieldDescriptor> fields_;
    const std::size_t depth_, total_fields_, name_bytes_;
};
}
