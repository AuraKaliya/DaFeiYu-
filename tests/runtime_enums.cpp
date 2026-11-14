#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/enum_value.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在枚举精度、定义或边界违约时报告验收失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 要求输入以指定异常类别拒绝，避免无关异常掩盖结果。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("enum input accepted"); }
// 验证枚举值的成员选择、数据计量、精确类型和共享描述寿命。
// 步骤：选择整数极值 → 核对分类和计量 → 拒绝未知值与错误种类 → 释放来源并验证最终回收。
void values() {
    const EnumMember members[] = {{"min", INT64_MIN}, {"max", INT64_MAX}};
    auto descriptor = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.value", 1, ValueKind::int64, members, 2, 16));
    auto enumeration = std::make_shared<const EnumValue>(EnumValue::create(descriptor, INT64_MIN));
    const auto value = Value::from_enum(enumeration);
    check(value.kind() == ValueKind::enumeration && value.payload_bytes() == 8 && value.node_count() == 1 &&
        value.as_enum().index() == 0 && std::get<std::int64_t>(value.as_enum().member().number) == INT64_MIN, "enum selection or accounting wrong");
    check(EnumValue::create(descriptor, INT64_MAX).index() == 1, "signed maximum enum lost");
    rejects<std::invalid_argument>([&] { (void)EnumValue::create(descriptor, std::int64_t{0}); });
    rejects<std::invalid_argument>([&] { (void)EnumValue::create(descriptor, std::uint64_t{0}); });
    rejects<std::invalid_argument>([] { (void)EnumValue::create({}, std::int64_t{0}); });
    rejects<std::invalid_argument>([] { (void)Value::from_enum({}); });
    rejects<std::bad_variant_access>([&] { (void)value.as_int64(); });
    rejects<std::bad_variant_access>([] { (void)Value::from_int64(1).as_enum(); });

    std::weak_ptr<const EnumDescriptor> observed;
    std::weak_ptr<const EnumValue> observed_value;
    {
        const EnumMember member{"max", UINT64_MAX};
        auto source = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.unsigned", 1, ValueKind::uint64, {&member,1}, 1, 16));
        auto selected = std::make_shared<const EnumValue>(EnumValue::create(source, UINT64_MAX));
        observed = source; observed_value = selected;
        const auto held = Value::from_enum(selected);
        source.reset(); selected.reset();
        check(!observed.expired() && !observed_value.expired() && std::get<std::uint64_t>(held.as_enum().member().number) == UINT64_MAX,
            "enum borrowed source or lost unsigned precision");
    }
    check(observed.expired() && observed_value.expired(), "enum retained after last owner");
}
}
// 验证整数精度、唯一成员、严格身份和不可变快照。
// 步骤：构造边界成员 → 核对顺序与输入隔离 → 拒绝非法定义和超限 → 比较完整定义。
int main() {
    try {
        values();
        std::vector<EnumMember> members{{"min", INT64_MIN}, {"zero", std::int64_t{0}}, {"max", INT64_MAX}};
        const auto type = EnumDescriptor::create("test.signed", UINT32_MAX, ValueKind::int64, members, 3, 21);
        check(type.version() == UINT32_MAX && type.name_bytes() == 21 && type.index_of(INT64_MIN) == 0 &&
            type.index_of(INT64_MAX) == 2 && !type.index_of(std::uint64_t{0}) && !type.index_of(std::int64_t{1}), "enum integer precision or kind lost");
        const auto same = EnumDescriptor::create("test.signed", UINT32_MAX, ValueKind::int64, members, 3, 21);
        members[0].name = "changed";
        check(type.members()[0].name == "min" && type.equivalent(same), "enum borrowed input or equivalence failed");
        const EnumMember unsigned_members[] = {{"zero", std::uint64_t{0}}, {"max", UINT64_MAX}};
        const auto wide = EnumDescriptor::create("test.unsigned", 1, ValueKind::uint64, unsigned_members, 2, 20);
        check(wide.index_of(UINT64_MAX) == 1 && !wide.index_of(std::int64_t{-1}) && !wide.equivalent(type), "unsigned enum narrowed");

        rejects<std::length_error>([&] { (void)EnumDescriptor::create("test.unsigned", 1, ValueKind::uint64, unsigned_members, 1, 20); });
        rejects<std::length_error>([&] { (void)EnumDescriptor::create("test.unsigned", 1, ValueKind::uint64, unsigned_members, 2, 19); });
        rejects<std::invalid_argument>([] { (void)EnumDescriptor::create("test.empty", 1, ValueKind::uint64, {}, 0, 20); });
        rejects<std::invalid_argument>([&] { (void)EnumDescriptor::create("test.unsigned", 1, ValueKind::int64, unsigned_members, 2, 20); });
        rejects<std::invalid_argument>([&] { (void)EnumDescriptor::create("test.unsigned", 1, ValueKind::float64, unsigned_members, 2, 20); });
        rejects<std::invalid_argument>([&] { (void)EnumDescriptor::create("test.unsigned", 0, ValueKind::uint64, unsigned_members, 2, 20); });
        for (const std::string& name : std::vector<std::string>{"unqualified", "Test.enum", "test..enum", "test.enum.", std::string("test.\0enum",10)})
            rejects<std::invalid_argument>([&] { (void)EnumDescriptor::create(name, 1, ValueKind::uint64, unsigned_members, 2, 64); });

        const EnumMember duplicate_name[] = {{"same", std::int64_t{1}}, {"same", std::int64_t{2}}};
        const EnumMember duplicate_number[] = {{"one", std::int64_t{1}}, {"alias", std::int64_t{1}}};
        const EnumMember invalid{"bad.name", std::int64_t{1}};
        rejects<std::invalid_argument>([&] { (void)EnumDescriptor::create("test.enum", 1, ValueKind::int64, duplicate_name, 2, 64); });
        rejects<std::invalid_argument>([&] { (void)EnumDescriptor::create("test.enum", 1, ValueKind::int64, duplicate_number, 2, 64); });
        rejects<std::invalid_argument>([&] { (void)EnumDescriptor::create("test.enum", 1, ValueKind::int64, {&invalid,1}, 2, 64); });
        std::vector<EnumMember> excessive(EnumDescriptor::maximum_members + 1, invalid);
        rejects<std::length_error>([&] { (void)EnumDescriptor::create("test.enum", 1, ValueKind::int64, excessive, excessive.size(), 64); });
        const EnumMember long_name{std::string(EnumDescriptor::maximum_name_bytes, 'a'), std::int64_t{1}};
        rejects<std::length_error>([&] { (void)EnumDescriptor::create("test.enum", 1, ValueKind::int64, {&long_name,1}, 1, SIZE_MAX); });

        const EnumMember reordered[] = {unsigned_members[1], unsigned_members[0]};
        check(!wide.equivalent(EnumDescriptor::create("test.unsigned", 1, ValueKind::uint64, reordered, 2, 20)) &&
            !wide.equivalent(EnumDescriptor::create("test.unsigned", 2, ValueKind::uint64, unsigned_members, 2, 20)), "enum identity or order ignored");
        std::cout << "Enum descriptor checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
