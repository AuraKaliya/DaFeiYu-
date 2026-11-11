#include <dafeiyu/value.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <iostream>
#include <type_traits>

namespace {
// 在基础值丢失类型、范围或寿命保证时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

// 验证生产基础值的精确类型、数值边界及独立引用寿命。
// 步骤：检查各数值类型 → 拒绝非有限数及错误读取 → 通过引用表共享不可变值并回收。
int main() {
    try {
        using namespace dfy::runtime;
        static_assert(!std::is_copy_assignable_v<Value> && std::is_copy_constructible_v<Value>);
        check(Value::from_bool(true).kind() == ValueKind::boolean && Value::from_bool(true).as_bool() &&
            !Value::from_bool(false).as_bool(), "Bool changed");
        for (auto input : {std::numeric_limits<std::int64_t>::min(), std::int64_t{0}, std::numeric_limits<std::int64_t>::max()}) {
            const auto value = Value::from_int64(input);
            check(value.kind() == ValueKind::int64 && value.as_int64() == input, "Int64 range lost");
        }
        for (auto input : {std::uint64_t{0}, std::numeric_limits<std::uint64_t>::max()}) {
            const auto value = Value::from_uint64(input);
            check(value.kind() == ValueKind::uint64 && value.as_uint64() == input, "UInt64 range lost");
        }
        for (auto input : {-0.0, 1.5, std::numeric_limits<double>::max(), std::numeric_limits<double>::denorm_min()}) {
            const auto value = Value::from_float64(input);
            check(value.kind() == ValueKind::float64 && value.as_float64() == input &&
                std::signbit(value.as_float64()) == std::signbit(input), "Float64 value changed");
        }
        for (auto input : {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
            std::numeric_limits<double>::quiet_NaN()}) {
            bool rejected = false;
            try { (void)Value::from_float64(input); } catch (const std::invalid_argument&) { rejected = true; }
            check(rejected, "nonfinite value accepted");
        }
        bool wrong_type = false;
        try { (void)Value::from_int64(1).as_float64(); } catch (const std::bad_variant_access&) { wrong_type = true; }
        check(wrong_type, "numeric read silently converted types");

        HandleRegistry<const Value> values(2);
        auto owned = std::make_shared<const Value>(Value::from_uint64(std::numeric_limits<std::uint64_t>::max()));
        const std::weak_ptr<const Value> lifetime = owned;
        const auto first = values.insert(owned);
        const auto second = values.retain(first);
        owned.reset();
        values.release(first);
        check(!lifetime.expired() && values.get(second).as_uint64() == std::numeric_limits<std::uint64_t>::max(),
            "shared host value lost");
        values.release(second);
        check(lifetime.expired(), "host value leaked");
        std::cout << "Runtime scalar value checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
