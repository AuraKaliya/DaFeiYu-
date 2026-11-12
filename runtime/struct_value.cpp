#include <dafeiyu/struct_value.hpp>
#include <unordered_map>
#include <algorithm>
#include <dafeiyu/array_value.hpp>

namespace dfy::runtime {

// 根据宿主描述构造独立不可变载荷，未通过校验时不返回半成品。
// 步骤：核对描述与字段数量 → 建立唯一名称索引 → 校验必填、种类及预算 → 保存有序字段及正文计量。
StructValue StructValue::create(std::shared_ptr<const StructDescriptor> descriptor,
    std::span<const NamedValue> fields, std::size_t max_payload_bytes, std::size_t max_nodes) {
    if (!descriptor) throw std::invalid_argument("struct: descriptor required");
    const auto node_limit = std::min(max_nodes, Value::maximum_nodes);
    if (!node_limit) throw std::length_error("struct: root node budget required");
    auto remaining_nodes = node_limit - 1;
    const auto declared = descriptor->fields();
    if (fields.size() > declared.size()) throw std::invalid_argument("struct: field count mismatch");
    std::unordered_map<std::string_view, const Value*> supplied;
    for (const auto& field : fields) {
        if (!supplied.emplace(field.name, &field.value).second) throw std::invalid_argument("struct: duplicate field");
    }

    auto remaining = max_payload_bytes;
    std::size_t matched = 0;
    for (const auto& field : declared) {
        const auto match = supplied.find(field.name);
        if (match == supplied.end()) {
            if (!field.optional) throw std::invalid_argument("struct: required field missing");
            continue;
        }
        ++matched;
        const auto& value = *match->second;
        if (value.kind() == ValueKind::null_value) {
            if (!field.nullable) throw std::invalid_argument("struct: null not allowed");
        } else if (value.kind() != field.kind) throw std::invalid_argument("struct: field kind mismatch");
        if (value.kind() == ValueKind::structure && !field.structure->equivalent(*value.as_structure().descriptor())) {
            throw std::invalid_argument("struct: nested definition mismatch");
        }
        if (value.kind() == ValueKind::array && !field.array->equivalent(*value.as_array().descriptor())) {
            throw std::invalid_argument("struct: array definition mismatch");
        }
        const auto nodes = value.node_count();
        if (nodes > remaining_nodes) throw std::length_error("struct: node limit exceeded");
        remaining_nodes -= nodes;
        const auto bytes = value.payload_bytes();
        if ((field.kind == ValueKind::string || field.kind == ValueKind::bytes) && bytes > field.max_bytes) {
            throw std::length_error("struct: field byte limit exceeded");
        }
        if (bytes > remaining) throw std::length_error("struct: payload byte limit exceeded");
        remaining -= bytes;
    }
    if (matched != supplied.size()) throw std::invalid_argument("struct: unknown field");

    std::vector<std::optional<Value>> values;
    values.reserve(declared.size());
    for (const auto& field : declared) {
        const auto found = supplied.find(field.name);
        if (found == supplied.end()) values.emplace_back(std::nullopt);
        else values.emplace_back(*found->second);
    }
    return StructValue(std::move(descriptor), std::move(values), max_payload_bytes - remaining, node_limit - remaining_nodes);
}

// 按描述字段名查找对应值，未匹配时不产生隐式字段或默认值。
const Value* StructValue::find(std::string_view name) const noexcept {
    const auto declared = descriptor_->fields();
    for (std::size_t index = 0; index < declared.size(); ++index) {
        if (declared[index].name == name) return fields_[index] ? &*fields_[index] : nullptr;
    }
    return nullptr;
}
}
