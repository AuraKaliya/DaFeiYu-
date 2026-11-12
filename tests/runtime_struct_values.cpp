#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/type_catalog.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <array>
#include <iostream>
#include <type_traits>

namespace {
using namespace dfy::runtime;
// 在结构载荷或描述寿命违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证非法构造或读取采用预期错误类别。
// 步骤：执行调用 → 捕获预期错误 → 对未拒绝的结果报告失败。
template<class Error, class Action>
void rejects(Action action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("struct: invalid input accepted");
}

// 构造独立测试描述，字段限制由调用方明确给出。
std::shared_ptr<const StructDescriptor> describe(std::span<const FieldDescriptor> fields) {
    return std::make_shared<const StructDescriptor>(StructDescriptor::create("app.event", 1, fields, {8, 128}));
}

// 验证六种基础字段、不同输入顺序、输入销毁及描述和载荷的独立持有。
// 步骤：登记描述并构造具名载荷 → 退出输入及目录作用域 → 反射读取 → 释放独立引用。
void verify_reflection_and_lifetime() {
    std::shared_ptr<const StructValue> value;
    std::weak_ptr<const StructDescriptor> type_lifetime;
    {
        TypeCatalog catalog(1, {6, 128});
        const FieldDescriptor fields[]{{"flag", ValueKind::boolean, 0}, {"signed", ValueKind::int64, 0},
            {"unsigned", ValueKind::uint64, 0}, {"ratio", ValueKind::float64, 0},
            {"text", ValueKind::string, 3}, {"data", ValueKind::bytes, 2}};
        const auto type = catalog.define(*describe(fields));
        type_lifetime = type;
        const std::array<std::uint8_t, 2> bytes{0, 255};
        std::vector<NamedValue> input{{"text", Value::from_string(std::string_view("A\0B", 3), 3)},
            {"data", Value::from_bytes(bytes, 2)}, {"unsigned", Value::from_uint64(UINT64_MAX)},
            {"ratio", Value::from_float64(-0.0)}, {"flag", Value::from_bool(true)}, {"signed", Value::from_int64(INT64_MIN)}};
        value = std::make_shared<const StructValue>(StructValue::create(type, input, 30));
        input[0].name = "changed";
    }
    const auto type = value->descriptor();
    check(value->size() == 6 && type->fields()[0].name == "flag" && value->at(0).as_bool(), "field order lost");
    check(value->find("signed")->as_int64() == INT64_MIN && value->find("unsigned")->as_uint64() == UINT64_MAX,
        "integer precision lost");
    check(std::signbit(value->find("ratio")->as_float64()) && value->find("data")->as_bytes()[1] == 255,
        "float or binary value changed");
    check(value->find("text")->as_string() == std::string_view("A\0B", 3) && !value->find("TEXT"), "text or name changed");
    rejects<std::out_of_range>([&] { (void)value->at(6); });
    rejects<std::bad_variant_access>([&] { (void)value->at(0).as_uint64(); });
    HandleRegistry<const StructValue> references(2);
    const auto first = references.insert(value), retained = references.retain(first);
    const std::weak_ptr<const StructValue> lifetime = value;
    value.reset();
    references.release(first);
    check(references.get(retained).find("text")->as_string().size() == 3, "retained struct lost");
    references.release(retained);
    check(lifetime.expired() && !type_lifetime.expired() && type->name() == "app.event", "type and value lifetime coupled");
}

// 验证字段完整性、精确种类、单字段及总正文上限与空结构。
// 步骤：构造合法边界 → 拒绝非法字段 → 验证两层字节预算 → 确认旧值保持。
void verify_rejections() {
    const FieldDescriptor fields[]{{"id", ValueKind::uint64, 0}, {"text", ValueKind::string, 2}};
    const auto type = describe(fields);
    const NamedValue good[]{{"id", Value::from_uint64(1)}, {"text", Value::from_string("ok", 2)}};
    const auto original = StructValue::create(type, good, 10);
    rejects<std::invalid_argument>([&] { (void)StructValue::create({}, good, 10); });
    rejects<std::invalid_argument>([&] { (void)StructValue::create(type, {}, 10); });
    for (const auto name : {"id", "other", "TEXT"}) {
        const NamedValue bad[]{{"id", Value::from_uint64(1)}, {name, Value::from_string("ok", 2)}};
        rejects<std::invalid_argument>([&] { (void)StructValue::create(type, bad, 10); });
    }
    const NamedValue wrong[]{{"id", Value::from_int64(1)}, {"text", Value::from_string("ok", 2)}};
    rejects<std::invalid_argument>([&] { (void)StructValue::create(type, wrong, 10); });
    const NamedValue long_text[]{{"id", Value::from_uint64(1)}, {"text", Value::from_string("bad", 3)}};
    rejects<std::length_error>([&] { (void)StructValue::create(type, long_text, 11); });
    rejects<std::length_error>([&] { (void)StructValue::create(type, good, 9); });
    check(StructValue::create(describe({}), {}, 0).size() == 0, "empty struct rejected");
    const FieldDescriptor binary[]{{"data", ValueKind::bytes, 0}};
    const std::array<std::uint8_t, 1> byte{0};
    const NamedValue nonempty[]{{"data", Value::from_bytes(byte, 1)}};
    rejects<std::length_error>([&] { (void)StructValue::create(describe(binary), nonempty, 1); });
    check(original.at(1).as_string() == "ok", "failed construction changed old value");
}
}

// 验收最小结构载荷及只读反射，不接入事件交付或插件服务。
// 步骤：核对不可变接口 → 验证反射与寿命 → 验证构造拒绝 → 汇报结果。
int main() {
    try {
        static_assert(!std::is_copy_assignable_v<StructValue>);
        static_assert(std::is_same_v<decltype(std::declval<StructValue&>().at(0)), const Value&>);
        verify_reflection_and_lifetime();
        verify_rejections();
        std::cout << "Runtime struct value checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
