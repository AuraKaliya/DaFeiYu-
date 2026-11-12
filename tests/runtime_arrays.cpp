#include <dafeiyu/array_value.hpp>
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/type_catalog.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在数组类型、顺序、寿命或预算不符合约定时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 只接受明确的拒绝类别，避免把其他异常当作成功验收。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("array input accepted"); }
// 建立数组约束的共享快照。
std::shared_ptr<const ArrayDescriptor> schema(ValueKind kind, std::size_t count, std::size_t bytes = 0, bool nullable = false) {
    return std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(kind, count, bytes, nullable));
}
// 覆盖全部标量、可空元素、边界、输入独立性和引用寿命。
// 步骤：逐类构造 → 验证空数组和 null → 拒绝类型及预算错误 → 通过引用表保留并回收。
void scalars() {
    const uint8_t bytes[]{0,255};
    const Value examples[] = {Value::from_bool(true), Value::from_int64(INT64_MIN), Value::from_uint64(UINT64_MAX),
        Value::from_float64(-0.0), Value::from_string(std::string("A\0B",3),3), Value::from_bytes(bytes,2)};
    for (const auto& value : examples) {
        const auto kind = value.kind();
        auto type = schema(kind, 1, kind == ValueKind::string || kind == ValueKind::bytes ? value.payload_bytes() : 0);
        const auto array = ArrayValue::create(type, {&value,1}, value.payload_bytes(), 2);
        check(array.size() == 1 && array.at(0).kind() == kind && array.node_count() == 2 &&
            array.payload_bytes() == value.payload_bytes(), "scalar array changed kind or accounting");
        rejects<std::out_of_range>([&] { (void)array.at(1); });
        rejects<std::length_error>([&] { (void)ArrayValue::create(type, {&value,1}, value.payload_bytes(), 1); });
    }
    auto empty_type = schema(ValueKind::string, 0, 0);
    check(ArrayValue::create(empty_type, {}, 0, 1).node_count() == 1, "empty array collapsed into absence");
    rejects<std::length_error>([&] { (void)ArrayValue::create(empty_type, {}, 0, 0); });
    const Value nullable[] = {Value::null(), Value::from_string({},0)};
    const auto nulls = ArrayValue::create(schema(ValueKind::string,2,0,true), nullable, 0);
    check(nulls.at(0).kind() == ValueKind::null_value && nulls.at(1).kind() == ValueKind::string && nulls.node_count() == 3,
        "array null and empty string collapsed");
    rejects<std::invalid_argument>([&] { (void)ArrayValue::create(schema(ValueKind::string,2), nullable, 0); });
    rejects<std::length_error>([&] { (void)ArrayValue::create(schema(ValueKind::string,1,0,true), nullable, 0); });
    rejects<std::invalid_argument>([&] { (void)ArrayValue::create(schema(ValueKind::uint64,1), {&examples[1],1}, 8); });
    rejects<std::length_error>([&] { (void)ArrayValue::create(schema(ValueKind::string,1,2), {&examples[4],1}, 3); });
    rejects<std::length_error>([&] { (void)ArrayValue::create(schema(ValueKind::string,1,3), {&examples[4],1}, 2); });
    rejects<std::invalid_argument>([] { (void)ArrayDescriptor::create(ValueKind::boolean,1,1); });
    for (auto kind : {ValueKind::structure, ValueKind::array, ValueKind::null_value, static_cast<ValueKind>(255)}) {
        rejects<std::invalid_argument>([&] { (void)ArrayDescriptor::create(kind,1); });
    }
    rejects<std::length_error>([] { (void)ArrayDescriptor::create(ValueKind::boolean,65537); });
    rejects<std::invalid_argument>([] { (void)Value::from_array({}); });
    std::vector<Value> input{examples[4]};
    auto array = std::make_shared<const ArrayValue>(ArrayValue::create(schema(ValueKind::string,1,3), input, 3));
    input.clear(); const std::weak_ptr<const ArrayValue> lifetime = array;
    HandleRegistry<const Value> refs(2);
    const auto original = refs.insert(std::make_shared<const Value>(Value::from_array(array)));
    const auto retained = refs.retain(original); array.reset(); refs.release(original);
    check(refs.get(retained).as_array().at(0).as_string() == std::string_view("A\0B",3), "array borrowed input or lost lifetime");
    refs.release(retained); check(lifetime.expired(), "array leaked");
}
// 验证结构内数组约束严格匹配，并让共享空数组的逻辑展开受硬预算限制。
// 步骤：构造数组字段 → 拒绝不同约束与深度 → 验证目录冲突 → 拒绝共享大数组的预算绕过。
void structures() {
    auto type = schema(ValueKind::uint64,2);
    const auto field = FieldDescriptor::array_field("items", type);
    auto parent = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.array",1,{&field,1},{1,64,2}));
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.array",1,{&field,1},{1,64}); });
    const Value element = Value::from_uint64(UINT64_MAX);
    auto array = std::make_shared<const ArrayValue>(ArrayValue::create(type,{&element,1},8));
    const NamedValue value{"items",Value::from_array(array)};
    check(StructValue::create(parent,{&value,1},8).node_count() == 3, "structure did not count array root");
    const auto different = FieldDescriptor::array_field("items", schema(ValueKind::uint64,3));
    auto other = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.array",1,{&different,1},{1,64,2}));
    rejects<std::invalid_argument>([&] { (void)StructValue::create(other,{&value,1},8); });
    TypeCatalog catalog(2,{2,128,2}); catalog.define(*parent);
    try { catalog.define(*other); check(false,"array constraint conflict accepted"); }
    catch (const DefinitionError& error) { check(error.reason() == DefinitionFailure::conflicting_definition,"wrong array conflict"); }
    auto large_type = schema(ValueKind::boolean,65536,0,true);
    std::vector<Value> nulls(65536, Value::null());
    auto large = std::make_shared<const ArrayValue>(ArrayValue::create(large_type,nulls,0));
    check(large->node_count() == 65537 && !large->payload_bytes(),"large empty array accounting wrong");
    std::vector<FieldDescriptor> fields; std::vector<NamedValue> values;
    for (unsigned i=0; i<17; ++i) {
        const auto name="a"+std::to_string(i);
        fields.push_back(FieldDescriptor::array_field(name,large_type)); values.push_back({name,Value::from_array(large)});
    }
    auto repeated = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.large",1,fields,{17,256,2}));
    rejects<std::length_error>([&] { (void)StructValue::create(repeated,values,0,std::numeric_limits<std::size_t>::max()); });
}
}
// 验收第一批标量数组，不把尚未实现的组合元素当作受支持类型。
int main() {
    try { scalars(); structures(); std::cout << "Scalar array checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
