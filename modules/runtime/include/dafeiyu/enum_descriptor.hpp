#pragma once
#include <dafeiyu/value.hpp>
#include <optional>

namespace dfy::runtime {
// 数字保留完整 64 位精度与符号，不通过浮点数或隐式符号转换表达成员。
using EnumNumber = std::variant<std::int64_t, std::uint64_t>;
struct EnumMember { std::string name; EnumNumber number; };

// 不可变闭合枚举：名称和取值均唯一，所有成员必须使用同一底层整数种类。
class EnumDescriptor {
  public:
    static constexpr std::size_t maximum_members = 65536;
    static constexpr std::size_t maximum_name_bytes = 4 * 1024 * 1024;
    // 校验非空成员集、精确整数种类、唯一性和预算，复制独立名称快照。
    static EnumDescriptor create(std::string_view name, std::uint32_t version, ValueKind underlying,
        std::span<const EnumMember> members, std::size_t max_members, std::size_t max_name_bytes);
    // 复制不可变枚举定义，不借用来源成员存储。
    EnumDescriptor(const EnumDescriptor&) = default;
    // 禁止修改已经发布的枚举定义。
    EnumDescriptor& operator=(const EnumDescriptor&) = delete;
    // 借用完整类型名称，使用期间须保持对应描述持有。
    std::string_view name() const noexcept { return name_; }
    // 返回精确契约版本，零版本在构造时拒绝。
    std::uint32_t version() const noexcept { return version_; }
    // 返回 Int64 或 UInt64，不将两者当作可互换的整数。
    ValueKind underlying() const noexcept { return underlying_; }
    // 按声明顺序借用成员；稳定数字由声明指定，不由索引产生。
    std::span<const EnumMember> members() const noexcept { return members_; }
    // 返回类型及成员名称的正文总字节数。
    std::size_t name_bytes() const noexcept { return name_bytes_; }
    // 按精确符号和数值查询成员索引，未知值或错误整数种类返回空值。
    std::optional<std::size_t> index_of(EnumNumber number) const noexcept;
    // 比较类型身份、整数种类及有序成员定义。
    bool equivalent(const EnumDescriptor& other) const noexcept;
  private:
    // 保存已校验的独立成员与身份，不再引用调用方内存。
    EnumDescriptor(std::string name, std::uint32_t version, ValueKind underlying,
        std::vector<EnumMember> members, std::size_t bytes)
        : name_(std::move(name)), version_(version), underlying_(underlying), members_(std::move(members)), name_bytes_(bytes) {}
    const std::string name_;
    const std::uint32_t version_;
    const ValueKind underlying_;
    const std::vector<EnumMember> members_;
    const std::size_t name_bytes_;
};
}
