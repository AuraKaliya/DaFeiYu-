#include <dafeiyu/type_catalog.hpp>
#include <algorithm>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>

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
    if (find_enum(descriptor.name(), descriptor.version()) || find_union(descriptor.name(), descriptor.version()))
        throw DefinitionError(DefinitionFailure::conflicting_definition);
    if (const auto existing = find(descriptor.name(), descriptor.version())) {
        const bool identical = existing->equivalent(descriptor);
        throw DefinitionError(identical ? DefinitionFailure::already_defined : DefinitionFailure::conflicting_definition);
    }
    if (size() == capacity_) throw DefinitionError(DefinitionFailure::exhausted);
    validate_dependencies(descriptor.fields());

    auto snapshot = std::make_shared<const StructDescriptor>(StructDescriptor::create(
        descriptor.name(), descriptor.version(), descriptor.fields(), limits_));
    definitions_.push_back(snapshot);
    return snapshot;
}
// 核对结构或联合引用的全部命名依赖，不自动登记缺少的子类型。
// 步骤：按实际种类查找依赖 → 区分缺失和种类冲突 → 比较完整定义。
void TypeCatalog::validate_dependencies(std::span<const FieldDescriptor> fields) const {
    for (const auto& field : fields) {
        if (field.array) validate_dependencies({&field.array->element().constraint(),1});
        if (field.map) validate_dependencies({&field.map->element().constraint(),1});
        if (field.enumeration) {
            const auto dependency = find_enum(field.enumeration->name(), field.enumeration->version());
            if (!dependency && (find(field.enumeration->name(), field.enumeration->version()) || find_union(field.enumeration->name(), field.enumeration->version())))
                throw DefinitionError(DefinitionFailure::conflicting_definition);
            if (!dependency) throw DefinitionError(DefinitionFailure::unknown_dependency);
            if (!dependency->equivalent(*field.enumeration)) throw DefinitionError(DefinitionFailure::conflicting_definition);
        }
        if (field.tagged_union) {
            const auto dependency = find_union(field.tagged_union->name(), field.tagged_union->version());
            if (!dependency && (find(field.tagged_union->name(), field.tagged_union->version()) || find_enum(field.tagged_union->name(), field.tagged_union->version())))
                throw DefinitionError(DefinitionFailure::conflicting_definition);
            if (!dependency) throw DefinitionError(DefinitionFailure::unknown_dependency);
            if (!dependency->equivalent(*field.tagged_union)) throw DefinitionError(DefinitionFailure::conflicting_definition);
        }
        if (!field.structure) continue;
        const auto dependency = find(field.structure->name(), field.structure->version());
        if (!dependency && (find_enum(field.structure->name(), field.structure->version()) || find_union(field.structure->name(), field.structure->version())))
            throw DefinitionError(DefinitionFailure::conflicting_definition);
        if (!dependency) throw DefinitionError(DefinitionFailure::unknown_dependency);
        if (!dependency->equivalent(*field.structure)) throw DefinitionError(DefinitionFailure::conflicting_definition);
    }
}
// 按精确身份查询枚举，不将同名结构当作枚举。
std::shared_ptr<const EnumDescriptor> TypeCatalog::find_enum(std::string_view name, std::uint32_t version) const noexcept {
    for (const auto& descriptor : enums_) if (descriptor->name() == name && descriptor->version() == version) return descriptor;
    return {};
}
// 登记有界枚举快照，失败保持目录及外部持有不变。
// 步骤：排除种类和定义冲突 → 核对共享容量 → 按目录预算复制 → 提交枚举。
std::shared_ptr<const EnumDescriptor> TypeCatalog::define(const EnumDescriptor& descriptor) {
    if (find(descriptor.name(), descriptor.version()) || find_union(descriptor.name(), descriptor.version()))
        throw DefinitionError(DefinitionFailure::conflicting_definition);
    if (const auto existing = find_enum(descriptor.name(), descriptor.version())) {
        throw DefinitionError(existing->equivalent(descriptor) ? DefinitionFailure::already_defined : DefinitionFailure::conflicting_definition);
    }
    if (size() == capacity_) throw DefinitionError(DefinitionFailure::exhausted);

    auto snapshot = std::make_shared<const EnumDescriptor>(EnumDescriptor::create(descriptor.name(), descriptor.version(),
        descriptor.underlying(), descriptor.members(), limits_.max_fields, limits_.max_name_bytes));
    enums_.push_back(snapshot);
    return snapshot;
}
// 查询联合的精确契约身份，不将其他种类视为兼容类型。
std::shared_ptr<const UnionDescriptor> TypeCatalog::find_union(std::string_view name, std::uint32_t version) const noexcept {
    for (const auto& descriptor : unions_) if (descriptor->name() == name && descriptor->version() == version) return descriptor;
    return {};
}
// 在依赖与预算完整通过后登记联合，失败保持所有原有快照。
// 步骤：核对身份和容量 → 检查全部分支依赖与预算 → 复制分支快照 → 提交目录。
std::shared_ptr<const UnionDescriptor> TypeCatalog::define(const UnionDescriptor& descriptor) {
    if (find(descriptor.name(),descriptor.version()) || find_enum(descriptor.name(),descriptor.version()))
        throw DefinitionError(DefinitionFailure::conflicting_definition);
    if (const auto existing = find_union(descriptor.name(),descriptor.version()))
        throw DefinitionError(existing->equivalent(descriptor) ? DefinitionFailure::already_defined : DefinitionFailure::conflicting_definition);
    if (size() == capacity_) throw DefinitionError(DefinitionFailure::exhausted);
    validate_dependencies(descriptor.branches());
    if (descriptor.total_fields() > limits_.max_fields || descriptor.name_bytes() > limits_.max_name_bytes || descriptor.depth() > limits_.max_depth)
        throw std::length_error("types: union descriptor limit exceeded");

    std::vector<UnionBranch> branches;
    for (std::size_t i = 0; i < descriptor.branches().size(); ++i) branches.push_back({descriptor.tag(i),descriptor.branches()[i]});
    auto snapshot = std::make_shared<const UnionDescriptor>(UnionDescriptor::create(descriptor.name(),descriptor.version(),branches,limits_));
    unions_.push_back(snapshot);
    return snapshot;
}
}
