#pragma once
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <variant>

namespace dfy::runtime {
// 宿主内部种类，不是 C ABI 或事件契约中的类型编号。
enum class ValueKind { boolean, int64, uint64, float64 };
static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559);

// 宿主持有的不可变基础值；当前只实现布尔和三种数值，不含插件析构代码。
class Value {
  public:
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
    // 复制产生独立的不可变值，不共享可修改存储。
    Value(const Value&) = default;
    // 禁止赋值，保持已经发布的值不变。
    Value& operator=(const Value&) = delete;
    // 返回与存储分支对应的宿主内部种类。
    ValueKind kind() const noexcept { return static_cast<ValueKind>(contents_.index()); }
    // 读取 Bool；类型不符抛出 bad_variant_access，不隐式转换。
    bool as_bool() const { return std::get<bool>(contents_); }
    // 读取 Int64，类型不符抛出 bad_variant_access。
    std::int64_t as_int64() const { return std::get<std::int64_t>(contents_); }
    // 读取 UInt64，类型不符抛出 bad_variant_access。
    std::uint64_t as_uint64() const { return std::get<std::uint64_t>(contents_); }
    // 读取 Float64，类型不符抛出 bad_variant_access。
    double as_float64() const { return std::get<double>(contents_); }
  private:
    using Contents = std::variant<bool, std::int64_t, std::uint64_t, double>;
    // 从已校验的明确类型保存只读内容，不向模块外暴露 variant 布局。
    explicit Value(Contents contents) : contents_(std::move(contents)) {}
    const Contents contents_;
};
}
