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
    if (find_enum(descriptor.name(), descriptor.version())) throw DefinitionError(DefinitionFailure::conflicting_definition);
    if (const auto existing = find(descriptor.name(), descriptor.version())) {
        const bool identical = existing->equivalent(descriptor);
        throw DefinitionError(identical ? DefinitionFailure::already_defined : DefinitionFailure::conflicting_definition);
    }
    if (size() == capacity_) throw DefinitionError(DefinitionFailure::exhausted);
    for (const auto& field : descriptor.fields()) {
        if (field.enumeration) {
            const auto dependency = find_enum(field.enumeration->name(), field.enumeration->version());
            if (!dependency && find(field.enumeration->name(), field.enumeration->version()))
                throw DefinitionError(DefinitionFailure::conflicting_definition);
            if (!dependency) throw DefinitionError(DefinitionFailure::unknown_dependency);
            if (!dependency->equivalent(*field.enumeration)) throw DefinitionError(DefinitionFailure::conflicting_definition);
        }
        if (!field.structure) continue;
        const auto dependency = find(field.structure->name(), field.structure->version());
        if (!dependency && find_enum(field.structure->name(), field.structure->version()))
            throw DefinitionError(DefinitionFailure::conflicting_definition);
        if (!dependency) throw DefinitionError(DefinitionFailure::unknown_dependency);
        if (!dependency->equivalent(*field.structure)) throw DefinitionError(DefinitionFailure::conflicting_definition);
    }

    auto snapshot = std::make_shared<const StructDescriptor>(StructDescriptor::create(
        descriptor.name(), descriptor.version(), descriptor.fields(), limits_));
    definitions_.push_back(snapshot);
    return snapshot;
}
// 按精确身份查询枚举，不将同名结构当作枚举。
std::shared_ptr<const EnumDescriptor> TypeCatalog::find_enum(std::string_view name, std::uint32_t version) const noexcept {
    for (const auto& descriptor : enums_) if (descriptor->name() == name && descriptor->version() == version) return descriptor;
    return {};
}
// 登记有界枚举快照，失败保持目录及外部持有不变。
// 步骤：排除种类和定义冲突 → 核对共享容量 → 按目录预算复制 → 提交枚举。
std::shared_ptr<const EnumDescriptor> TypeCatalog::define(const EnumDescriptor& descriptor) {
    if (find(descriptor.name(), descriptor.version())) throw DefinitionError(DefinitionFailure::conflicting_definition);
    if (const auto existing = find_enum(descriptor.name(), descriptor.version())) {
        throw DefinitionError(existing->equivalent(descriptor) ? DefinitionFailure::already_defined : DefinitionFailure::conflicting_definition);
    }
    if (size() == capacity_) throw DefinitionError(DefinitionFailure::exhausted);

    auto snapshot = std::make_shared<const EnumDescriptor>(EnumDescriptor::create(descriptor.name(), descriptor.version(),
        descriptor.underlying(), descriptor.members(), limits_.max_fields, limits_.max_name_bytes));
    enums_.push_back(snapshot);
    return snapshot;
}
}
