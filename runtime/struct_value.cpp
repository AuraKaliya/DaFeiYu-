#include <dafeiyu/struct_value.hpp>
#include <unordered_map>

namespace dfy::runtime {
namespace {
// 计算值的逻辑正文字节数，嵌套使用已核定正文，不包含分配器和描述开销。
std::size_t scalar_payload_bytes(const Value& value) {
    switch (value.kind()) {
    case ValueKind::boolean: return 1;
    case ValueKind::int64:
    case ValueKind::uint64:
    case ValueKind::float64: return 8;
    case ValueKind::string: return value.as_string().size();
    case ValueKind::bytes: return value.as_bytes().size();
    case ValueKind::structure: return value.as_structure().payload_bytes();
    case ValueKind::null_value: return 0;
    }
    throw std::invalid_argument("struct: unsupported value kind");
}
}

// 根据宿主描述构造独立不可变载荷，未通过校验时不返回半成品。
// 步骤：核对描述与字段数量 → 建立唯一名称索引 → 校验必填、种类及预算 → 保存有序字段及正文计量。
StructValue StructValue::create(std::shared_ptr<const StructDescriptor> descriptor,
    std::span<const NamedValue> fields, std::size_t max_payload_bytes) {
    if (!descriptor) throw std::invalid_argument("struct: descriptor required");
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
            continue;
        }
        if (value.kind() != field.kind) throw std::invalid_argument("struct: field kind mismatch");
        if (field.kind == ValueKind::structure && !field.structure->equivalent(*value.as_structure().descriptor())) {
            throw std::invalid_argument("struct: nested definition mismatch");
        }
        const auto bytes = scalar_payload_bytes(value);
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
    return StructValue(std::move(descriptor), std::move(values), max_payload_bytes - remaining);
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
