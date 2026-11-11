#include <dafeiyu/value.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <iostream>
#include <type_traits>
#include <array>

namespace {
// 在基础值丢失类型、范围或寿命保证时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 验证可变输入被复制，UTF-8 边界严格校验且字节限制不被空值绕过。
// 步骤：检查有效文本及二进制 → 拒绝非法编码 → 检查字节上限与错误类型。
void verify_text_and_bytes() {
    using namespace dfy::runtime;
    std::string input("A\0汉字", 8);
    const auto text = Value::from_string(input, input.size());
    input.assign("changed");
    check(text.kind() == ValueKind::string && text.as_string() == std::string("A\0汉字", 8), "text borrowed input");
    check(Value::from_string({}, 0).as_string().empty() && Value::from_bytes({}, 0).as_bytes().empty(), "empty value lost");
    for (const auto valid : {"\xc2\x80", "\xdf\xbf", "\xe0\xa0\x80", "\xed\x9f\xbf", "\xef\xbf\xbf",
        "\xf0\x90\x80\x80", "\xf4\x8f\xbf\xbf"}) {
        check(Value::from_string(valid, 4).as_string() == valid, "valid UTF-8 boundary rejected");
    }
    for (const auto invalid : {"\x80", "\xc0\x80", "\xc1\xbf", "\xc2", "\xe0\x9f\xbf", "\xed\xa0\x80",
        "\xe1\x80", "\xe1\x41\x80", "\xf0\x8f\xbf\xbf", "\xf4\x90\x80\x80", "\xf5\x80\x80\x80", "\xff"}) {
        bool rejected = false;
        try { (void)Value::from_string(invalid, 4); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "malformed UTF-8 accepted");
    }
    std::array<std::uint8_t, 3> input_bytes{0, 0xff, 0x80};
    const auto binary = Value::from_bytes(input_bytes, 3);
    input_bytes.fill(1);
    check(binary.kind() == ValueKind::bytes && binary.as_bytes().size() == 3 && binary.as_bytes()[0] == 0 &&
        binary.as_bytes()[1] == 0xff && binary.as_bytes()[2] == 0x80, "binary changed or interpreted as text");
    bool text_limit = false, byte_limit = false, wrong_type = false;
    try { (void)Value::from_string("汉", 2); } catch (const std::length_error&) { text_limit = true; }
    try { (void)Value::from_bytes(input_bytes, 2); } catch (const std::length_error&) { byte_limit = true; }
    try { (void)binary.as_string(); } catch (const std::bad_variant_access&) { wrong_type = true; }
    check(text_limit && byte_limit && wrong_type, "byte limit or type boundary bypassed");
}
}

// 验证生产基础值的精确类型、数值与文本边界及独立引用寿命。
// 步骤：检查数值类型 → 拒绝错误读取 → 验证文本和字节 → 通过引用表共享并回收。
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
        verify_text_and_bytes();

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
        const auto text_reference = values.insert(std::make_shared<const Value>(Value::from_string("host text", 9)));
        const auto text_retained = values.retain(text_reference);
        values.release(text_reference);
        check(values.get(text_retained).as_string() == "host text", "retained text view lost");
        values.release(text_retained);
        std::cout << "Runtime value checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
