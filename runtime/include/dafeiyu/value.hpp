#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>
#include <memory>

namespace dfy::runtime {
// 宿主内部种类，不是 C ABI 或事件契约中的类型编号。
enum class ValueKind { boolean, int64, uint64, float64, string, bytes, structure, null_value, array, map, enumeration };
class StructValue;
class ArrayValue;
class MapValue;
class EnumValue;
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

// 宿主持有的不可变值，基础正文独立存储，组合内容共享宿主快照，不含插件析构代码。
class Value {
  public:
    static constexpr std::size_t maximum_nodes = 1024 * 1024;
    // 创建明确为 Bool 的值，不将其他数字分支当作布尔读取。
    static Value from_bool(bool value) { return Value(value); }
    // 创建保留完整有符号 64 位范围的值。
    static Value from_int64(std::int64_t value) { return Value(value); }
    // 创建保留完整无符号 64 位范围的值。
    static Value from_uint64(std::uint64_t value) { return Value(value); }
    // 创建有限 Float64，保留负零和非规格化值，拒绝 NaN 与无穷。
    // 步骤：校验有限性 → 保存原始双精度值。
    static Value from_float64(double value) {
        if (!std::isfinite(value)) throw std::invalid_argument("value: finite Float64 required");
        return Value(value);
    }
    // 复制不超过明确字节上限的 UTF-8 文本，非法编码或超限时拒绝。
    static Value from_string(std::string_view text, std::size_t max_bytes);
    // 复制不超过明确字节上限的任意二进制数据，零字节和空数据均可表达。
    static Value from_bytes(std::span<const std::uint8_t> bytes, std::size_t max_bytes);
    // 保持非空不可变结构，嵌套内容不会引用插件代码或可变容器。
    static Value from_structure(std::shared_ptr<const StructValue> value);
    // 创建显式空值，不等同于字段缺失、空文本或空字节。
    static Value null() { return Value(std::monostate{}); }
    // 保持非空不可变数组，不允许空指针代替 null 或空数组。
    static Value from_array(std::shared_ptr<const ArrayValue> value);
    // 保持非空不可变映射，不以空指针代替空映射或 null。
    static Value from_map(std::shared_ptr<const MapValue> value);
    // 保持非空不可变枚举值，不将整数隐式当作枚举。
    static Value from_enum(std::shared_ptr<const EnumValue> value);
    // 复制基础存储或共享不可变组合内容，不共享可修改内容。
    Value(const Value&) = default;
    // 禁止赋值，保持已经发布的值不变。
    Value& operator=(const Value&) = delete;
    // 返回与存储分支对应的宿主内部种类。
    ValueKind kind() const noexcept { return static_cast<ValueKind>(contents_.index()); }
    // 返回逻辑正文大小，组合值使用构造时核定的聚合计量。
    std::size_t payload_bytes() const;
    // 返回逻辑数据节点数，缺失不形成值，null 与空内容各占一个节点。
    std::size_t node_count() const;
    // 读取 Bool；类型不符抛出 bad_variant_access，不隐式转换。
    bool as_bool() const { return std::get<bool>(contents_); }
    // 读取 Int64，类型不符抛出 bad_variant_access。
    std::int64_t as_int64() const { return std::get<std::int64_t>(contents_); }
    // 读取 UInt64，类型不符抛出 bad_variant_access。
    std::uint64_t as_uint64() const { return std::get<std::uint64_t>(contents_); }
    // 读取 Float64，类型不符抛出 bad_variant_access。
    double as_float64() const { return std::get<double>(contents_); }
    // 借用 UTF-8 文本，仅在该值的对应持有引用有效期间使用；类型不符抛错。
    std::string_view as_string() const { return std::get<std::string>(contents_); }
    // 借用只读字节，仅在该值的对应持有引用有效期间使用；类型不符抛错。
    std::span<const std::uint8_t> as_bytes() const { return std::get<Bytes>(contents_); }
    // 借用嵌套结构，必须保持对应值引用有效；错误种类抛出 bad_variant_access。
    const StructValue& as_structure() const { return *std::get<std::shared_ptr<const StructValue>>(contents_); }
    // 借用数组内容，必须保持对应值引用有效。
    const ArrayValue& as_array() const { return *std::get<std::shared_ptr<const ArrayValue>>(contents_); }
    // 借用映射内容，借用期依赖对应值持有。
    const MapValue& as_map() const { return *std::get<std::shared_ptr<const MapValue>>(contents_); }
    // 借用枚举值，种类错误抛出 bad_variant_access，借用依赖对应持有。
    const EnumValue& as_enum() const { return *std::get<std::shared_ptr<const EnumValue>>(contents_); }
  private:
    using Bytes = std::vector<std::uint8_t>;
    using Contents = std::variant<bool, std::int64_t, std::uint64_t, double, std::string, Bytes, std::shared_ptr<const StructValue>, std::monostate,
        std::shared_ptr<const ArrayValue>, std::shared_ptr<const MapValue>, std::shared_ptr<const EnumValue>>;
    // 从已校验的明确类型保存只读内容，不向模块外暴露 variant 布局。
    explicit Value(Contents contents) : contents_(std::move(contents)) {}
    const Contents contents_;
};
}
