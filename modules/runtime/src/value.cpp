#include <dafeiyu/value.hpp>
#include <dafeiyu/utf8.hpp>
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/union_value.hpp>

namespace dfy::runtime {
// 接收宿主已构造的不可变结构，不允许空指针伪装成结构或 null。
Value Value::from_structure(std::shared_ptr<const StructValue> value) {
    if (!value) throw std::invalid_argument("value: nonempty structure required");
    return Value(std::move(value));
}
// 保存已构造数组的不可变持有，不接受空数组指针。
Value Value::from_array(std::shared_ptr<const ArrayValue> value) {
    if (!value) throw std::invalid_argument("value: nonempty array required");
    return Value(std::move(value));
}
// 保存不可变映射持有，不接受空指针。
Value Value::from_map(std::shared_ptr<const MapValue> value) {
    if (!value) throw std::invalid_argument("value: nonempty map required");
    return Value(std::move(value));
}
// 保存不可变枚举持有，不接受空指针或隐式整数。
Value Value::from_enum(std::shared_ptr<const EnumValue> value) {
    if (!value) throw std::invalid_argument("value: nonempty enum required");
    return Value(std::move(value));
}
// 保存已构造的联合值，不接受空指针。
Value Value::from_union(std::shared_ptr<const UnionValue> value) {
    if (!value) throw std::invalid_argument("value: nonempty union required");
    return Value(std::move(value));
}
// 读取已确定类型的逻辑正文计量，不遍历不可变子结构。
std::size_t Value::payload_bytes() const {
    switch (kind()) {
    case ValueKind::boolean: return 1;
    case ValueKind::int64:
    case ValueKind::uint64:
    case ValueKind::float64:
    case ValueKind::enumeration: return 8;
    case ValueKind::string: return as_string().size();
    case ValueKind::bytes: return as_bytes().size();
    case ValueKind::structure: return as_structure().payload_bytes();
    case ValueKind::null_value: return 0;
    case ValueKind::array: return as_array().payload_bytes();
    case ValueKind::map: return as_map().payload_bytes();
    case ValueKind::tagged_union: return as_union().payload_bytes();
    }
    throw std::invalid_argument("value: unsupported kind");
}
// 组合共享按每次出现计费，避免空正文掩盖递归遍历成本。
std::size_t Value::node_count() const {
    switch (kind()) {
    case ValueKind::structure: return as_structure().node_count();
    case ValueKind::array: return as_array().node_count();
    case ValueKind::map: return as_map().node_count();
    case ValueKind::tagged_union: return as_union().node_count();
    case ValueKind::boolean:
    case ValueKind::int64:
    case ValueKind::uint64:
    case ValueKind::float64:
    case ValueKind::string:
    case ValueKind::bytes:
    case ValueKind::null_value:
    case ValueKind::enumeration: return 1;
    }
    throw std::invalid_argument("value: unsupported kind");
}
// 在上限内保存独立 UTF-8 文本，允许内嵌零且不执行规范化或截断。
// 步骤：核对字节上限 → 校验完整编码 → 复制到宿主不可变存储。
Value Value::from_string(std::string_view text, std::size_t max_bytes) {
    if (text.size() > max_bytes) throw std::length_error("value: string byte limit exceeded");
    if (!valid_utf8(text)) throw std::invalid_argument("value: valid UTF-8 required");
    return Value(text.empty() ? std::string{} : std::string(text));
}

// 在上限内保存独立二进制数据，不借用输入缓冲。
// 步骤：核对字节上限 → 按空或非空输入构造宿主存储。
Value Value::from_bytes(std::span<const std::uint8_t> bytes, std::size_t max_bytes) {
    if (bytes.size() > max_bytes) throw std::length_error("value: binary byte limit exceeded");
    return Value(bytes.empty() ? Bytes{} : Bytes(bytes.begin(), bytes.end()));
}
}
