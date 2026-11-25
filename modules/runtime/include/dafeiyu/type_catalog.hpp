#pragma once
#include <dafeiyu/struct_descriptor.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_descriptor.hpp>
#include <memory>

namespace dfy::runtime {
enum class DefinitionFailure { already_defined, conflicting_definition, exhausted, unknown_dependency };
class DefinitionError : public std::runtime_error {
  public:
    // 保存类型登记失败类别，不借用来源描述或插件内存。
    explicit DefinitionError(DefinitionFailure reason) : std::runtime_error("types: definition rejected"), reason_(reason) {}
    // 返回可供宿主诊断及后续 ABI 映射使用的失败类别。
    DefinitionFailure reason() const noexcept { return reason_; }
  private:
    DefinitionFailure reason_;
};

// 同一宿主线程使用的有界类型目录，目录退出不使外部持有的描述快照失效。
class TypeCatalog {
  public:
    // 建立空目录，明确类型数量与单份描述限制；类型容量必须为正。
    TypeCatalog(std::size_t capacity, DescriptorLimits limits);
    // 释放目录持有的描述，外部共享快照继续由其持有者管理。
    ~TypeCatalog() = default;
    // 禁止复制目录的定义责任。
    TypeCatalog(const TypeCatalog&) = delete;
    // 禁止赋值或隐式移动目录，保持登记边界稳定。
    TypeCatalog& operator=(const TypeCatalog&) = delete;
    // 核对全部命名依赖已经登记且定义相同，再保存快照；重复、冲突和容量不足均拒绝。
    std::shared_ptr<const StructDescriptor> define(const StructDescriptor& descriptor);
    // 登记独立枚举快照，与结构共享身份空间及类型数量预算。
    std::shared_ptr<const EnumDescriptor> define(const EnumDescriptor& descriptor);
    // 核对全部分支依赖后登记联合，与其他命名类型共享身份和容量。
    std::shared_ptr<const UnionDescriptor> define(const UnionDescriptor& descriptor);
    // 按完整名称和精确版本查询独立持有的快照，未登记的键返回空值。
    std::shared_ptr<const StructDescriptor> find(std::string_view name, std::uint32_t version) const noexcept;
    // 查询精确枚举身份，其他类型或未登记身份返回空值。
    std::shared_ptr<const EnumDescriptor> find_enum(std::string_view name, std::uint32_t version) const noexcept;
    // 按完整名称和版本查询联合，其他种类或未知身份返回空值。
    std::shared_ptr<const UnionDescriptor> find_union(std::string_view name, std::uint32_t version) const noexcept;
    // 返回当前登记的类型版本数量，不包含外部引用数量。
    std::size_t size() const noexcept { return definitions_.size() + enums_.size() + unions_.size(); }

  private:
    // 核对全部字段或分支依赖，缺少依赖与同键不同定义分别拒绝。
    void validate_dependencies(std::span<const FieldDescriptor> fields) const;
    const std::size_t capacity_;
    const DescriptorLimits limits_;
    std::vector<std::shared_ptr<const StructDescriptor>> definitions_;
    std::vector<std::shared_ptr<const EnumDescriptor>> enums_;
    std::vector<std::shared_ptr<const UnionDescriptor>> unions_;
};
}
