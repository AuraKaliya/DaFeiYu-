#pragma once
#include <dafeiyu/struct_descriptor.hpp>
#include <optional>

namespace dfy::runtime {
// 分支以显式 UInt64 标签识别，名称及内容约束沿用字段描述，分支不能缺失。
struct UnionBranch { std::uint64_t tag; FieldDescriptor value; };
// 不可变闭合联合，所有分支共同参与定义预算，实际值只能选择其中一个。
class UnionDescriptor {
  public:
    // 校验非空分支、唯一标签及内容约束，深度和展开预算沿用结构描述硬上限。
    static UnionDescriptor create(std::string_view name, std::uint32_t version,
        std::span<const UnionBranch> branches, DescriptorLimits limits);
    // 复制标签并共享已经独立保存的不可变分支快照。
    UnionDescriptor(const UnionDescriptor&) = default;
    // 禁止修改已发布的分支或标签。
    UnionDescriptor& operator=(const UnionDescriptor&) = delete;
    // 借用完整联合名称，须保持对应描述引用。
    std::string_view name() const noexcept { return schema_->name(); }
    // 返回精确联合契约版本。
    std::uint32_t version() const noexcept { return schema_->version(); }
    // 按声明顺序借用分支名称和内容约束，可空不等同于可选。
    std::span<const FieldDescriptor> branches() const noexcept { return schema_->fields(); }
    // 返回分支的显式标签，越界抛出 out_of_range，索引不代替稳定标签。
    std::uint64_t tag(std::size_t index) const { return tags_.at(index); }
    // 返回包括本联合及所有分支嵌套的最大描述深度。
    std::size_t depth() const noexcept { return schema_->depth(); }
    // 返回按分支展开的字段、分支及枚举成员总数。
    std::size_t total_fields() const noexcept { return schema_->total_fields(); }
    // 返回联合名称及全部分支描述展开后的名称字节数。
    std::size_t name_bytes() const noexcept { return schema_->name_bytes(); }
    // 查询稳定标签的分支索引，未知标签返回空值，不选择默认分支。
    std::optional<std::size_t> index_of(std::uint64_t tag) const noexcept;
    // 比较身份、声明顺序、标签及全部分支约束。
    bool equivalent(const UnionDescriptor& other) const noexcept { return tags_ == other.tags_ && schema_->equivalent(*other.schema_); }
  private:
    // 保持已验证的独立分支快照与稳定标签，不向外暴露内部结构描述身份。
    UnionDescriptor(std::shared_ptr<const StructDescriptor> schema, std::vector<std::uint64_t> tags)
        : schema_(std::move(schema)), tags_(std::move(tags)) {}
    const std::shared_ptr<const StructDescriptor> schema_;
    const std::vector<std::uint64_t> tags_;
};
}
