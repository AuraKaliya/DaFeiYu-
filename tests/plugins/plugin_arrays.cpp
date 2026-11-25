#include <dafeiyu/host_services.hpp>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/union_value.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_array_api_v2) == 40 && offsetof(dfy_v2_array_api_v2,element_type) == 32);
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
// 在组合数组的版本、反射或持有违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 精确查询能力及布局，不把其他版本视为兼容。
// 步骤：查询版本 → 核对公共前缀 → 返回类型化函数表。
template<class T>
const T& service(std::string_view name, uint32_t version = 1) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},version,&table,nullptr) == DFY_V2_OK && table,"array service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == version,"array table mismatch");
    return *static_cast<const T*>(table);
}
}
// 验证版本 2 的组合数组读取、独立元素描述和叶值父链持有。
// 步骤：构造可空结构数组 → 读取描述和内容 → 拒绝旧版本与错误输出 → 释放父链再读取叶值及空数组。
int inspection() {
    try {
        const FieldDescriptor text{"text",ValueKind::string,3};
        const auto record = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&text,1},{1,32}));
        const NamedValue data{"text",Value::from_string(std::string("a\0b",3),3)};
        const auto contents = std::make_shared<const StructValue>(StructValue::create(record,{&data,1},3));
        const auto type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ElementDescriptor::create({"",ValueKind::structure,0,record,false,true},{2,32,2}),3));
        const Value elements[] = {Value::from_structure(contents),Value::null()};
        const auto field = FieldDescriptor::array_field("items",type);
        const auto parent = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.event",1,{&field,1},{3,64,3}));
        const NamedValue input{"items",Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(type,elements,3,4)))};
        auto payload = std::make_shared<const StructValue>(StructValue::create(parent,{&input,1},3,5));
        const std::weak_ptr<const StructValue> lifetime = payload;
        HostServices host(12); const auto root = host.expose(payload); payload.reset(); auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        const auto& arrays = service<dfy_v2_array_api_v2>(DFY_V2_ARRAY_INTERFACE,2);
        const auto& legacy = service<dfy_v2_array_api>(DFY_V2_ARRAY_INTERFACE);
        const auto& constraints = service<dfy_v2_constraint_api_v2>(DFY_V2_CONSTRAINT_INTERFACE,2);
        const auto& structs = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
        const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
        dfy_v2_handle schema{},array{},element_type{},named{},element{},structure{},held{},temporary{},rejected{};
        dfy_v2_array_type_info declared{}; dfy_v2_array_info actual{}; dfy_v2_field_info_v2 info{}; dfy_v2_value_view view{};
        check(structs.descriptor(root,&schema,nullptr) == DFY_V2_OK && arrays.field_type(schema,0,&declared,nullptr) == DFY_V2_OK &&
            declared.element_kind == DFY_V2_VALUE_STRUCTURE && declared.element_flags == DFY_V2_ELEMENT_NULLABLE && declared.max_elements == 3,"array declared metadata wrong");
        check(structs.field(root,0,&array,nullptr) == DFY_V2_OK && arrays.describe(array,&actual,nullptr) == DFY_V2_OK && actual.size == 2 &&
            actual.type.element_kind == DFY_V2_VALUE_STRUCTURE && arrays.element_type(array,&element_type,nullptr) == DFY_V2_OK,"array actual metadata wrong");
        check(constraints.describe(element_type,&info,nullptr) == DFY_V2_OK && info.kind == DFY_V2_VALUE_STRUCTURE && !info.name.length &&
            info.flags == DFY_V2_FIELD_NULLABLE && constraints.named_type(element_type,&named,nullptr) == DFY_V2_OK,"array full element schema missing");
        check(arrays.element(array,0,&element,nullptr) == DFY_V2_OK && values.structure(element,&structure,nullptr) == DFY_V2_OK &&
            structs.field(structure,0,&held,nullptr) == DFY_V2_OK,"array leaf access failed");
        check(arrays.element(array,1,&temporary,nullptr) == DFY_V2_OK && values.read(temporary,&view,nullptr) == DFY_V2_OK && view.kind == DFY_V2_VALUE_NULL,"array null lost");
        refs.release(temporary,nullptr);
        check(legacy.describe(array,&actual,nullptr) == DFY_V2_UNSUPPORTED && !actual.type.element_kind &&
            legacy.field_type(schema,0,&declared,nullptr) == DFY_V2_UNSUPPORTED && !declared.element_kind &&
            legacy.element(array,1,&rejected,nullptr) == DFY_V2_UNSUPPORTED && !rejected.id,"legacy array accepted composite declaration");
        check(arrays.element(array,2,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id &&
            arrays.element_type(held,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id &&
            arrays.element_type(array,nullptr,nullptr) == DFY_V2_INVALID_ARGUMENT,"array invalid output accepted");
        const void* unavailable = &arrays;
        check(HostServices::query({DFY_V2_ARRAY_INTERFACE,sizeof(DFY_V2_ARRAY_INTERFACE)-1},3,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable,"array version mismatch accepted");
        std::vector<dfy_v2_handle> filled;
        while (host.references() < 12) { dfy_v2_handle copy{}; check(refs.retain(named,&copy,nullptr) == DFY_V2_OK,"array capacity setup failed"); filled.push_back(copy); }
        check(arrays.element_type(array,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id &&
            arrays.element(array,0,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && host.references() == 12,"array allocation failure leaked");
        for (const auto copy : filled) refs.release(copy,nullptr);

        refs.release(root,nullptr); refs.release(array,nullptr); refs.release(element,nullptr); refs.release(structure,nullptr);
        check(!lifetime.expired() && values.read(held,&view,nullptr) == DFY_V2_OK && std::string_view(view.data.string.data,view.data.string.length) == std::string_view("a\0b",3),"array leaf lost parent");
        refs.release(held,nullptr);
        check(lifetime.expired() && constraints.describe(element_type,&info,nullptr) == DFY_V2_OK,"array metadata retained payload");
        refs.release(schema,nullptr); refs.release(element_type,nullptr); refs.release(named,nullptr);
        check(!host.references(),"array references leaked");
        const auto empty = host.expose(std::make_shared<const Value>(Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(type,{},0,1)))));
        check(arrays.describe(empty,&actual,nullptr) == DFY_V2_OK && !actual.size && actual.type.element_kind == DFY_V2_VALUE_STRUCTURE &&
            arrays.element_type(empty,&element_type,nullptr) == DFY_V2_OK && constraints.describe(element_type,&info,nullptr) == DFY_V2_OK &&
            info.kind == DFY_V2_VALUE_STRUCTURE,"empty array lost complete element schema");
        check(arrays.element(empty,0,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"empty array synthesized content");
        refs.release(empty,nullptr); refs.release(element_type,nullptr);
        check(!host.references(),"empty array metadata leaked");
        std::cout << "Composite C array checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
namespace {
// 通过真实 C DLL 消费各类组合数组，最后叶引用保持完整事件预算。
// 步骤：构造元素类型矩阵 → 有界 C 读取 → 解开实际内容并释放父链 → 核对空数组与部分失败回收。
void consume(const dfy_test_composite_arrays_api& consumer) {
    const auto text = Value::from_string(std::string("a\0b",3),3); const auto integer = Value::from_uint64(UINT64_MAX);
    const FieldDescriptor field{"text",ValueKind::string,3};
    const auto record = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&field,1},{1,32}));
    const NamedValue record_input{"text",text}; const auto structure = std::make_shared<const StructValue>(StructValue::create(record,{&record_input,1},3));
    const EnumMember member{"min",INT64_MIN};
    const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.enum",1,ValueKind::int64,{&member,1},1,32));
    const UnionBranch branch{UINT64_MAX,{"number",ValueKind::uint64,0}};
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.choice",1,{&branch,1},{1,32}));
    const auto inner = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::uint64,1));
    const auto map = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::string,1,1,3));
    const MapEntry entry{"k",text};
    struct Example { FieldDescriptor schema; Value value; uint32_t kind; };
    const Example examples[] = {{{"",ValueKind::string,3},text,DFY_V2_VALUE_STRING},
        {{"",ValueKind::structure,0,record},Value::from_structure(structure),DFY_V2_VALUE_STRUCTURE},
        {FieldDescriptor::enum_field("",enumeration),Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(enumeration,INT64_MIN))),DFY_V2_VALUE_ENUM},
        {FieldDescriptor::union_field("",choice),Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,UINT64_MAX,integer,16))),DFY_V2_VALUE_UNION},
        {FieldDescriptor::array_field("",inner),Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(inner,{&integer,1},8))),DFY_V2_VALUE_ARRAY},
        {FieldDescriptor::map_field("",map),Value::from_map(std::make_shared<const MapValue>(MapValue::create(map,{&entry,1},4))),DFY_V2_VALUE_MAP}};
    for (const auto& example : examples) {
        auto constraint = example.schema; constraint.nullable = true;
        const auto array_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ElementDescriptor::create(constraint,{16,256,8}),2));
        const Value elements[] = {example.value,Value::null()};
        const auto array_field = FieldDescriptor::array_field("items",array_type);
        const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.event",1,{&array_field,1},{32,512,16}));
        const NamedValue input{"items",Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(array_type,elements,64)))};
        const auto payload = std::make_shared<const StructValue>(StructValue::create(type,{&input,1},64));
        auto store = std::make_shared<dfy::events::EventStore>(4,64);
        auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store,1);
        auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue,1);
        HostServices host(16); const auto receiver = host.expose(reader); auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
        const auto& arrays = service<dfy_v2_array_api_v2>(DFY_V2_ARRAY_INTERFACE,2);
        const auto& constraints = service<dfy_v2_constraint_api_v2>(DFY_V2_CONSTRAINT_INTERFACE,2);
        const auto& structs = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
        const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
        dfy_v2_handle request{},root{},held{},metadata{},temporary{},next{}; dfy_test_composite_array_counts count{};
        check(reads.next(receiver,&request,nullptr) == DFY_V2_OK,"array event read failed");
        const auto event = store->accept(payload); queue->push(event); store->release(event); host.advance();
        check(reads.payload(request,&root,nullptr) == DFY_V2_OK,"array event root missing");
        check(consumer.inspect(HostServices::query,root,0,1,&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.size && !held.id && host.references() == 3,"C array budget failure leaked");
        check(consumer.inspect(HostServices::query,root,0,2,&count,&held,nullptr) == DFY_V2_OK && count.size == 2 && count.nulls == 1 &&
            count.kind == example.kind && count.flags == DFY_V2_FIELD_NULLABLE && host.references() == 4,"C composite array reflection failed");
        check(structs.field(root,0,&temporary,nullptr) == DFY_V2_OK && arrays.element_type(temporary,&metadata,nullptr) == DFY_V2_OK,"array metadata hold failed");
        refs.release(temporary,nullptr);
        if (example.kind == DFY_V2_VALUE_ARRAY) check(arrays.element(held,0,&next,nullptr) == DFY_V2_OK,"nested array leaf failed");
        if (example.kind == DFY_V2_VALUE_MAP) check(service<dfy_v2_map_api>(DFY_V2_MAP_INTERFACE).find(held,{"k",1},&next,nullptr) == DFY_V2_OK,"array map leaf failed");
        if (example.kind == DFY_V2_VALUE_UNION) check(service<dfy_v2_union_api>(DFY_V2_UNION_INTERFACE).content(held,&next,nullptr) == DFY_V2_OK,"array union leaf failed");
        if (example.kind == DFY_V2_VALUE_STRUCTURE) {
            check(values.structure(held,&temporary,nullptr) == DFY_V2_OK && structs.field(temporary,0,&next,nullptr) == DFY_V2_OK,"array structure leaf failed");
            refs.release(temporary,nullptr);
        }
        if (next.id) { refs.release(held,nullptr); held = next; }
        refs.release(root,nullptr); refs.release(request,nullptr); host.advance(); dfy_v2_value_view view{};
        check(store->references() == 1 && reader->size() == 1 && values.read(held,&view,nullptr) == DFY_V2_OK,"array leaf escaped event budget");
        if (view.kind == DFY_V2_VALUE_STRING) check(std::string_view(view.data.string.data,view.data.string.length) == text.as_string(),"array leaf text changed");
        if (view.kind == DFY_V2_VALUE_UINT64) check(view.data.uint64 == UINT64_MAX,"array leaf integer narrowed");
        if (view.kind == DFY_V2_VALUE_ENUM) { dfy_v2_enum_selection selected{}; check(service<dfy_v2_enum_api>(DFY_V2_ENUM_INTERFACE).read(held,&selected,nullptr) == DFY_V2_OK && selected.number.data.int64 == INT64_MIN,"array enum lost"); }
        refs.release(held,nullptr); host.advance(); dfy_v2_field_info_v2 info{};
        check(!store->references() && !reader->size() && constraints.describe(metadata,&info,nullptr) == DFY_V2_OK && info.kind == example.kind,"array metadata retained event budget");
        refs.release(metadata,nullptr); reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver,nullptr);
        const NamedValue empty_input{"items",Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(array_type,{},0)))};
        const auto empty = host.expose(std::make_shared<const StructValue>(StructValue::create(type,{&empty_input,1},0)));
        check(consumer.inspect(HostServices::query,empty,0,0,&count,&held,nullptr) == DFY_V2_OK && !count.size && !held.id && count.kind == example.kind,"empty C array lost declaration");
        refs.release(empty,nullptr); check(!host.references() && !host.resources(),"C array resources leaked");
        {
            HostServices full(3); const auto source = full.expose(payload); auto scope = full.bind();
            check(consumer.inspect(HostServices::query,source,0,2,&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.size && !held.id && full.references() == 1,"C array partial acquisition leaked");
            refs.release(source,nullptr);
        }
    }
}
}
// 串联生产服务边界及真实 C DLL 的完整组合数组消费。
// 步骤：验收生产访问 → 查询纯 C 消费者 → 验证元素矩阵和事件预算链。
int wmain(int argc, wchar_t** argv) {
    if (inspection()) return 1;
    try {
        check(argc == 2,"C composite array consumer DLL required");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap,"C array bootstrap failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_COMPOSITE_ARRAYS_ID,sizeof(DFY_TEST_COMPOSITE_ARRAYS_ID)-1},1,&table,nullptr) == DFY_V2_OK && table,"C array consumer missing");
        const auto& consumer = *static_cast<const dfy_test_composite_arrays_api*>(table);
        check(consumer.header.struct_size == sizeof(consumer) && consumer.header.interface_version == 1 && consumer.inspect,"C array consumer invalid");
        consume(consumer); std::cout << "Real C composite array checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
