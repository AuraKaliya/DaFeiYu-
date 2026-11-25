#include <dafeiyu/union_descriptor.hpp>
#include <algorithm>
#include <unordered_set>

namespace dfy::runtime {
// 建立闭合带标签联合，复用已有描述验证器核定全部分支的约束与展开预算。
// 步骤：检查非空分支及容量 → 拒绝可选分支和重复标签 → 校验完整分支描述 → 保存不可变快照。
UnionDescriptor UnionDescriptor::create(std::string_view name, std::uint32_t version,
    std::span<const UnionBranch> branches, DescriptorLimits limits) {
    if (branches.empty()) throw std::invalid_argument("union: nonempty branches required");
    if (branches.size() > std::min(limits.max_fields, StructDescriptor::maximum_fields))
        throw std::length_error("union: branch limit exceeded");
    auto remaining_names = std::min(limits.max_name_bytes, StructDescriptor::maximum_name_bytes);
    if (name.size() > remaining_names) throw std::length_error("union: name byte limit exceeded");
    remaining_names -= name.size();
    std::vector<FieldDescriptor> fields;
    std::vector<std::uint64_t> tags;
    std::unordered_set<std::uint64_t> unique;
    fields.reserve(branches.size()); tags.reserve(branches.size());
    for (const auto& branch : branches) {
        if (branch.value.name.size() > remaining_names) throw std::length_error("union: name byte limit exceeded");
        remaining_names -= branch.value.name.size();
        if (branch.value.optional) throw std::invalid_argument("union: branch payload cannot be missing");
        if (!unique.insert(branch.tag).second) throw std::invalid_argument("union: duplicate tag");
        fields.push_back(branch.value); tags.push_back(branch.tag);
    }

    auto schema = std::make_shared<const StructDescriptor>(StructDescriptor::create(name,version,fields,limits));
    return UnionDescriptor(std::move(schema),std::move(tags));
}
// 按完整 UInt64 匹配标签，零和最大值均可作为声明标签。
std::optional<std::size_t> UnionDescriptor::index_of(std::uint64_t tag) const noexcept {
    for (std::size_t i = 0; i < tags_.size(); ++i) if (tags_[i] == tag) return i;
    return {};
}
}
