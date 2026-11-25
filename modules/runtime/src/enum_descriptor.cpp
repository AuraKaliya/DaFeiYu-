#include <dafeiyu/enum_descriptor.hpp>
#include "descriptor_name.hpp"
#include <algorithm>
#include <set>
#include <unordered_set>

namespace dfy::runtime {
// 建立有界闭合枚举，保留用户声明的整数精度和成员顺序。
// 步骤：校验类型身份与硬上限 → 核对成员名称、数字种类及唯一性 → 保存独立快照。
EnumDescriptor EnumDescriptor::create(std::string_view name, std::uint32_t version, ValueKind underlying,
    std::span<const EnumMember> members, std::size_t max_members, std::size_t max_name_bytes) {
    if (!version || !detail::valid_descriptor_name(name, true) || members.empty())
        throw std::invalid_argument("enum: identity and nonempty members required");
    if (underlying != ValueKind::int64 && underlying != ValueKind::uint64)
        throw std::invalid_argument("enum: Int64 or UInt64 required");
    if (members.size() > std::min(max_members, maximum_members)) throw std::length_error("enum: member limit exceeded");
    const auto limit = std::min(max_name_bytes, maximum_name_bytes);
    if (name.size() > limit) throw std::length_error("enum: name byte limit exceeded");
    auto remaining = limit - name.size();

    std::unordered_set<std::string_view> names;
    std::set<EnumNumber> numbers;
    for (const auto& member : members) {
        if (!detail::valid_descriptor_name(member.name, false)) throw std::invalid_argument("enum: invalid member name");
        if (member.name.size() > remaining) throw std::length_error("enum: name byte limit exceeded");
        remaining -= member.name.size();
        if (std::holds_alternative<std::int64_t>(member.number) != (underlying == ValueKind::int64))
            throw std::invalid_argument("enum: integer kind mismatch");
        if (!names.insert(member.name).second || !numbers.insert(member.number).second)
            throw std::invalid_argument("enum: duplicate member name or number");
    }

    return EnumDescriptor(std::string(name), version, underlying, {members.begin(), members.end()}, limit - remaining);
}
// 查找已声明数字，不接受隐式符号转换或未声明的位组合。
std::optional<std::size_t> EnumDescriptor::index_of(EnumNumber number) const noexcept {
    for (std::size_t i = 0; i < members_.size(); ++i) if (members_[i].number == number) return i;
    return {};
}
// 按完整身份与声明顺序比较定义，成员重排属于不同定义。
// 步骤：比较身份、种类和成员数 → 逐项核对名称与数字。
bool EnumDescriptor::equivalent(const EnumDescriptor& other) const noexcept {
    if (this == &other) return true;
    if (name_ != other.name_ || version_ != other.version_ || underlying_ != other.underlying_ || members_.size() != other.members_.size()) return false;
    for (std::size_t i = 0; i < members_.size(); ++i)
        if (members_[i].name != other.members_[i].name || members_[i].number != other.members_[i].number) return false;
    return true;
}
}
