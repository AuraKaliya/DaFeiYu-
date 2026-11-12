#pragma once
#include <dafeiyu/struct_descriptor.hpp>
#include <memory>

namespace dfy::runtime {
enum class DefinitionFailure { already_defined, conflicting_definition, exhausted };
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
    // 复制并登记新定义；重复、冲突和容量不足均拒绝，不覆盖既有记录。
    std::shared_ptr<const StructDescriptor> define(const StructDescriptor& descriptor);
    // 按完整名称和精确版本查询独立持有的快照，未登记的键返回空值。
    std::shared_ptr<const StructDescriptor> find(std::string_view name, std::uint32_t version) const noexcept;
    // 返回当前登记的类型版本数量，不包含外部引用数量。
    std::size_t size() const noexcept { return definitions_.size(); }

  private:
    const std::size_t capacity_;
    const DescriptorLimits limits_;
    std::vector<std::shared_ptr<const StructDescriptor>> definitions_;
};
}
