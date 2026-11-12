#include <dafeiyu/struct_descriptor.hpp>
#include <unordered_set>

namespace dfy::runtime {
namespace {
// 检查小写 ASCII 标识；类型名至少包含两个点分段，字段名只允许一个段。
// 步骤：按段校验首字母及后续字符 → 拒绝空段 → 核对是否为完整类型名。
bool valid_name(std::string_view name, bool qualified) {
    bool first = true, dotted = false;
    for (const char character : name) {
        if (character == '.' && qualified && !first) {
            first = true;
            dotted = true;
            continue;
        }
        if (!(character >= 'a' && character <= 'z') &&
            (first || !((character >= '0' && character <= '9') || character == '_'))) return false;
        first = false;
    }
    return !first && (!qualified || dotted);
}

// 从名称预算中扣除实际字节，先比较再相减以避免累计长度溢出。
void consume_name(std::string_view name, std::size_t& remaining) {
    if (name.size() > remaining) throw std::length_error("descriptor: name byte limit exceeded");
    remaining -= name.size();
}
}

// 生成宿主独立持有的最小结构描述，全部字段必填且不提供空值或嵌套占位。
// 步骤：核对容量与类型身份 → 校验字段名称、种类和约束 → 拒绝重复字段 → 复制不可变快照。
StructDescriptor StructDescriptor::create(std::string_view name, std::uint32_t version,
    std::span<const FieldDescriptor> fields, DescriptorLimits limits) {
    if (fields.size() > limits.max_fields) throw std::length_error("descriptor: field limit exceeded");
    auto remaining = limits.max_name_bytes;
    consume_name(name, remaining);
    if (!version || !valid_name(name, true)) throw std::invalid_argument("descriptor: invalid type identity");

    std::unordered_set<std::string_view> names;
    for (const auto& field : fields) {
        consume_name(field.name, remaining);
        if (!valid_name(field.name, false)) throw std::invalid_argument("descriptor: invalid field name");
        switch (field.kind) {
        case ValueKind::string:
        case ValueKind::bytes:
            break;
        case ValueKind::boolean:
        case ValueKind::int64:
        case ValueKind::uint64:
        case ValueKind::float64:
            if (field.max_bytes) throw std::invalid_argument("descriptor: byte limit on numeric field");
            break;
        default:
            throw std::invalid_argument("descriptor: unsupported field kind");
        }
        if (!names.insert(field.name).second) throw std::invalid_argument("descriptor: duplicate field");
    }

    return StructDescriptor(std::string(name), version, {fields.begin(), fields.end()});
}
}
