#include <dafeiyu/array_value.hpp>
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/type_catalog.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <dafeiyu/union_value.hpp>
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
    auto parent = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.array",1,{&field,1},{2,64,2}));
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.array",1,{&field,1},{1,64}); });
    const Value element = Value::from_uint64(UINT64_MAX);
    auto array = std::make_shared<const ArrayValue>(ArrayValue::create(type,{&element,1},8));
    const NamedValue value{"items",Value::from_array(array)};
    check(StructValue::create(parent,{&value,1},8).node_count() == 3, "structure did not count array root");

    const auto different = FieldDescriptor::array_field("items", schema(ValueKind::uint64,3));
    auto other = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.array",1,{&different,1},{2,64,2}));
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
    auto repeated = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.large",1,fields,{34,256,2}));
    rejects<std::length_error>([&] { (void)StructValue::create(repeated,values,0,std::numeric_limits<std::size_t>::max()); });
}
// 验证完整组合元素、逻辑重复计量与穿过数组的命名依赖。
// 步骤：构造结构数组 → 核对共享预算和完整定义 → 验证嵌套数组及联合 → 拒绝身份回返并登记依赖。
void composites() {
    const FieldDescriptor text{"text",ValueKind::string,3};
    const auto record = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&text,1},{1,32}));
    const NamedValue input{"text",Value::from_string("abc",3)};
    const auto value = std::make_shared<const StructValue>(StructValue::create(record,{&input,1},3));
    const auto constraint = ElementDescriptor::create({"",ValueKind::structure,0,record,false,true},{2,32,2});
    const auto type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(constraint,3));
    const Value elements[] = {Value::from_structure(value),Value::null(),Value::from_structure(value)};
    const auto array = std::make_shared<const ArrayValue>(ArrayValue::create(type,elements,6,6));
    check(array->size() == 3 && array->node_count() == 6 && array->payload_bytes() == 6 && type->depth() == 2 &&
        type->total_fields() == 2 && array->at(0).as_structure().at(0).as_string() == "abc","composite array accounting wrong");
    rejects<std::length_error>([&] { (void)ArrayValue::create(type,elements,5); });
    rejects<std::length_error>([&] { (void)ArrayValue::create(type,elements,6,5); });
    const FieldDescriptor changed{"text",ValueKind::string,4};
    const auto other = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&changed,1},{1,32}));
    const Value mismatch = Value::from_structure(std::make_shared<const StructValue>(StructValue::create(other,{&input,1},3)));
    rejects<std::invalid_argument>([&] { (void)ArrayValue::create(type,{&mismatch,1},3); });
    const auto different = ArrayDescriptor::create(ElementDescriptor::create({"",ValueKind::structure,0,other,false,true},{2,32,2}),3);
    check(!type->equivalent(different),"array ignored full element definition");
    const auto outer = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::array_field("",type),{3,32,3}),2));
    const Value shared[] = {Value::from_array(array),Value::from_array(array)};
    const auto nested = ArrayValue::create(outer,shared,12,13);
    check(nested.node_count() == 13 && nested.payload_bytes() == 12 && outer->depth() == 3,"nested array accounting lost");
    rejects<std::length_error>([&] { (void)ArrayValue::create(outer,shared,12,12); });

    const UnionBranch branch{UINT64_MAX,{"number",ValueKind::uint64,0}};
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.choice",1,{&branch,1},{1,32}));
    const auto choices = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::union_field("",choice),{2,32,2}),1));
    const Value selected = Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,UINT64_MAX,Value::from_uint64(UINT64_MAX),16)));
    check(ArrayValue::create(choices,{&selected,1},16,3).at(0).as_union().tag() == UINT64_MAX,"union array lost stable tag");
    const auto field = FieldDescriptor::array_field("items",type);
    const auto parent = StructDescriptor::create("test.parent",1,{&field,1},{3,64,3});
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.record",1,{&field,1},{3,64,3}); });
    const FieldDescriptor conflicts[] = {field,{"other",ValueKind::structure,0,other}};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.parent",1,conflicts,{5,128,3}); });
    TypeCatalog catalog(2,{4,128,3});
    try { catalog.define(parent); check(false,"array dependency bypassed catalog"); }
    catch (const DefinitionError& error) { check(error.reason() == DefinitionFailure::unknown_dependency && !catalog.size(),"wrong array dependency rejection"); }
    catalog.define(*record); catalog.define(parent);
    check(catalog.size() == 2,"array dependency registration failed");

    auto deep = schema(ValueKind::boolean,0);
    for (unsigned depth = 2; depth <= 64; ++depth)
        deep = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ElementDescriptor::create(FieldDescriptor::array_field("",deep),{64,0,64}),0));
    check(deep->depth() == 64 && deep->total_fields() == 64,"anonymous array layers escaped descriptor budget");
    rejects<std::length_error>([&] { (void)ElementDescriptor::create(FieldDescriptor::array_field("",deep),{65,0,64}); });
}
}
// 验收标量及组合数组的完整内容约束、展开预算和命名依赖。
// 步骤：验收基础元素 → 核对父结构约束 → 验收组合及嵌套 → 报告结果。
int main() {
    try { scalars(); structures(); composites(); std::cout << "Scalar and composite array checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
