#include "field_validation.hpp"
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/union_value.hpp>

namespace dfy::runtime::detail {
// 统一结构字段和联合分支的内容验证，不隐式转换种类或放宽子描述约束。
// 步骤：区分 null 与正常种类 → 比较完整子定义 → 核对文本或字节上限。
void validate_field(const FieldDescriptor& field, const Value& value) {
    if (value.kind() == ValueKind::null_value) {
        if (!field.nullable) throw std::invalid_argument("field: null not allowed");
        return;
    }
    if (value.kind() != field.kind) throw std::invalid_argument("field: kind mismatch");
    if (value.kind() == ValueKind::structure && !field.structure->equivalent(*value.as_structure().descriptor()))
        throw std::invalid_argument("field: nested definition mismatch");
    if (value.kind() == ValueKind::array && !field.array->equivalent(*value.as_array().descriptor()))
        throw std::invalid_argument("field: array definition mismatch");
    if (value.kind() == ValueKind::map && !field.map->equivalent(*value.as_map().descriptor()))
        throw std::invalid_argument("field: map definition mismatch");
    if (value.kind() == ValueKind::enumeration && !field.enumeration->equivalent(*value.as_enum().descriptor()))
        throw std::invalid_argument("field: enum definition mismatch");
    if (value.kind() == ValueKind::tagged_union && !field.tagged_union->equivalent(*value.as_union().descriptor()))
        throw std::invalid_argument("field: union definition mismatch");

    if ((field.kind == ValueKind::string || field.kind == ValueKind::bytes) && value.payload_bytes() > field.max_bytes)
        throw std::length_error("field: byte limit exceeded");
}
}
