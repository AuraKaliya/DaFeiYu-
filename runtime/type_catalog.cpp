#include <dafeiyu/type_catalog.hpp>
#include <algorithm>

namespace dfy::runtime {
// 记录类型数与单份描述上限，拒绝无法容纳任何定义的目录容量。
TypeCatalog::TypeCatalog(std::size_t capacity, DescriptorLimits limits) : capacity_(capacity), limits_(limits) {
    if (!capacity) throw std::invalid_argument("types: positive capacity required");
}

// 查询精确类型键，不创建描述、不自动转换版本或修改目录。
std::shared_ptr<const StructDescriptor> TypeCatalog::find(std::string_view name, std::uint32_t version) const noexcept {
    for (const auto& descriptor : definitions_) {
        if (descriptor->name() == name && descriptor->version() == version) return descriptor;
    }
    return {};
}

// 在全部约束通过后登记独立描述，失败保持目录及已有快照不变。
// 步骤：检查同键定义 → 核对容量与已登记子定义 → 按描述限制构造快照 → 提交新记录。
std::shared_ptr<const StructDescriptor> TypeCatalog::define(const StructDescriptor& descriptor) {
    if (const auto existing = find(descriptor.name(), descriptor.version())) {
        const bool identical = existing->equivalent(descriptor);
        throw DefinitionError(identical ? DefinitionFailure::already_defined : DefinitionFailure::conflicting_definition);
    }
    if (definitions_.size() == capacity_) throw DefinitionError(DefinitionFailure::exhausted);
    for (const auto& field : descriptor.fields()) {
        if (!field.structure) continue;
        const auto dependency = find(field.structure->name(), field.structure->version());
        if (!dependency) throw DefinitionError(DefinitionFailure::unknown_dependency);
        if (!dependency->equivalent(*field.structure)) throw DefinitionError(DefinitionFailure::conflicting_definition);
    }

    auto snapshot = std::make_shared<const StructDescriptor>(StructDescriptor::create(
        descriptor.name(), descriptor.version(), descriptor.fields(), limits_));
    definitions_.push_back(snapshot);
    return snapshot;
}
}
