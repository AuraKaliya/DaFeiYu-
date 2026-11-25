#include <dafeiyu/host_services.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/union_value.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_map_api_v2) == 48 && offsetof(dfy_v2_map_api_v2,value_type) == 40);
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
// 在组合映射的版本、精确键或引用边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 精确查询版本和布局，不把较大的旧表当作新能力。
// 步骤：查询目标版本 → 核对公共前缀 → 返回类型化表。
template<class T>
const T& service(std::string_view name, uint32_t version = 1) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},version,&table,nullptr) == DFY_V2_OK && table,"map service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == version,"map table mismatch");
    return *static_cast<const T*>(table);
}
}
// 验证完整映射的反射、精确键查找、双输出原子性及叶值持有。
// 步骤：构造可空结构映射 → 读取约束和数据 → 拒绝旧版与错误输入 → 释放父链并验证空映射声明。
int inspection() {
    try {
        const FieldDescriptor text{"text",ValueKind::string,3};
        const auto record = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&text,1},{1,32}));
        const NamedValue content{"text",Value::from_string(std::string("a\0b",3),3)};
        const auto structure = std::make_shared<const StructValue>(StructValue::create(record,{&content,1},3));
        const auto type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ElementDescriptor::create({"",ValueKind::structure,0,record,false,true},{2,32,2}),3,3));
        const MapEntry entries[] = {{"",Value::from_structure(structure)},{std::string("a\0b",3),Value::from_structure(structure)},{"nil",Value::null()}};
        const auto field = FieldDescriptor::map_field("items",type);
        const auto parent = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.event",1,{&field,1},{3,64,3}));
        const NamedValue input{"items",Value::from_map(std::make_shared<const MapValue>(MapValue::create(type,entries,12,9)))};
        auto payload = std::make_shared<const StructValue>(StructValue::create(parent,{&input,1},12,10));
        const std::weak_ptr<const StructValue> lifetime = payload;
        HostServices host(16); const auto root = host.expose(payload); payload.reset(); auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        const auto& maps = service<dfy_v2_map_api_v2>(DFY_V2_MAP_INTERFACE,2);
        const auto& legacy = service<dfy_v2_map_api>(DFY_V2_MAP_INTERFACE);
        const auto& constraints = service<dfy_v2_constraint_api_v2>(DFY_V2_CONSTRAINT_INTERFACE,2);
        const auto& structs = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
        const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
        dfy_v2_handle schema{},map{},value_type{},named{},element{},child{},held{},temporary{},rejected{};
        dfy_v2_map_type_info declared{}; dfy_v2_map_info actual{}; dfy_v2_field_info_v2 info{}; dfy_v2_value_view view{}; dfy_v2_string_view key{};
        check(structs.descriptor(root,&schema,nullptr) == DFY_V2_OK && maps.field_type(schema,0,&declared,nullptr) == DFY_V2_OK &&
            declared.value_kind == DFY_V2_VALUE_STRUCTURE && declared.value_flags == DFY_V2_ELEMENT_NULLABLE && declared.max_key_bytes == 3,"map declared metadata wrong");
        check(structs.field(root,0,&map,nullptr) == DFY_V2_OK && maps.describe(map,&actual,nullptr) == DFY_V2_OK && actual.size == 3 &&
            maps.value_type(map,&value_type,nullptr) == DFY_V2_OK && constraints.describe(value_type,&info,nullptr) == DFY_V2_OK &&
            info.kind == DFY_V2_VALUE_STRUCTURE && !info.name.length && info.flags == DFY_V2_FIELD_NULLABLE && constraints.named_type(value_type,&named,nullptr) == DFY_V2_OK,
            "map full value schema missing");
        check(maps.entry(map,1,&key,&element,nullptr) == DFY_V2_OK && std::string_view(key.data,key.length) == std::string_view("a\0b",3) &&
            maps.find(map,key,&temporary,nullptr) == DFY_V2_OK && temporary.id != element.id,"map embedded-zero lookup or ownership wrong");
        refs.release(temporary,nullptr);
        check(values.structure(element,&child,nullptr) == DFY_V2_OK && structs.field(child,0,&held,nullptr) == DFY_V2_OK,"map leaf access failed");
        check(maps.find(map,{nullptr,0},&temporary,nullptr) == DFY_V2_OK,"map empty key lost"); refs.release(temporary,nullptr);
        check(maps.find(map,{"nil",3},&temporary,nullptr) == DFY_V2_OK && values.read(temporary,&view,nullptr) == DFY_V2_OK && view.kind == DFY_V2_VALUE_NULL,"map null lost"); refs.release(temporary,nullptr);
        check(maps.find(map,{"a",1},&rejected,nullptr) == DFY_V2_NOT_FOUND && !rejected.id &&
            maps.find(map,{nullptr,1},&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"map key boundary lost");
        check(legacy.describe(map,&actual,nullptr) == DFY_V2_UNSUPPORTED && !actual.type.value_kind &&
            legacy.field_type(schema,0,&declared,nullptr) == DFY_V2_UNSUPPORTED && !declared.value_kind &&
            legacy.entry(map,2,&key,&rejected,nullptr) == DFY_V2_UNSUPPORTED && !key.data && !rejected.id &&
            legacy.find(map,{"nil",3},&rejected,nullptr) == DFY_V2_UNSUPPORTED && !rejected.id,"legacy map accepted composite declaration");
        check(maps.entry(map,UINT64_MAX,&key,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !key.data && !rejected.id &&
            maps.value_type(held,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"map invalid output accepted");
        const void* unavailable = &maps;
        check(HostServices::query({DFY_V2_MAP_INTERFACE,sizeof(DFY_V2_MAP_INTERFACE)-1},3,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable,"map version mismatch accepted");
        std::vector<dfy_v2_handle> filled;
        while (host.references() < 16) { dfy_v2_handle copy{}; check(refs.retain(named,&copy,nullptr) == DFY_V2_OK,"map capacity setup failed"); filled.push_back(copy); }
        key = {"old",3};
        check(maps.entry(map,0,&key,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !key.data && !key.length && !rejected.id &&
            maps.find(map,{nullptr,0},&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id &&
            maps.value_type(map,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && host.references() == 16,"map allocation partially committed");
        for (const auto copy : filled) refs.release(copy,nullptr);

        refs.release(root,nullptr); refs.release(map,nullptr); refs.release(element,nullptr); refs.release(child,nullptr);
        check(!lifetime.expired() && values.read(held,&view,nullptr) == DFY_V2_OK && std::string_view(view.data.string.data,view.data.string.length) == std::string_view("a\0b",3),"map leaf lost parent ownership");
        refs.release(held,nullptr);
        check(lifetime.expired() && constraints.describe(value_type,&info,nullptr) == DFY_V2_OK,"map value metadata retained data");
        refs.release(schema,nullptr); refs.release(value_type,nullptr); refs.release(named,nullptr);
        check(!host.references(),"map references leaked");
        const auto empty = host.expose(std::make_shared<const Value>(Value::from_map(std::make_shared<const MapValue>(MapValue::create(type,{},0,1)))));
        check(maps.describe(empty,&actual,nullptr) == DFY_V2_OK && !actual.size && actual.type.value_kind == DFY_V2_VALUE_STRUCTURE &&
            maps.value_type(empty,&value_type,nullptr) == DFY_V2_OK && constraints.describe(value_type,&info,nullptr) == DFY_V2_OK && info.kind == DFY_V2_VALUE_STRUCTURE,"empty map lost complete declaration");
        check(maps.entry(empty,0,&key,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !key.data && !rejected.id &&
            maps.find(empty,{nullptr,0},&rejected,nullptr) == DFY_V2_NOT_FOUND && !rejected.id,"empty map synthesized value");
        refs.release(empty,nullptr); refs.release(value_type,nullptr);
        check(!host.references(),"empty map metadata leaked");
        std::cout << "Composite C map checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
namespace {
// 由真实 C DLL 枚举完整映射，并验证交替容器内的叶数据保持原事件预算。
// 步骤：构造完整值矩阵 → C 反射和精确查键 → 解开叶数据并释放父链 → 验证空映射和失败回收。
void consume(const dfy_test_composite_maps_api& consumer) {
    const auto text = Value::from_string(std::string("a\0b",3),3); const auto integer = Value::from_uint64(UINT64_MAX);
    const FieldDescriptor field{"text",ValueKind::string,3};
    const auto record = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&field,1},{1,32}));
    const NamedValue record_input{"text",text}; const auto structure = std::make_shared<const StructValue>(StructValue::create(record,{&record_input,1},3));
    const EnumMember member{"min",INT64_MIN};
    const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.enum",1,ValueKind::int64,{&member,1},1,32));
    const UnionBranch branch{UINT64_MAX,{"number",ValueKind::uint64,0}};
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.choice",1,{&branch,1},{1,32}));
    const auto inner_map = std::make_shared<const MapDescriptor>(MapDescriptor::create(ElementDescriptor::create({"",ValueKind::structure,0,record},{2,32,2}),1,1));
    const MapEntry entry{"k",Value::from_structure(structure)};
    const auto nested_map = Value::from_map(std::make_shared<const MapValue>(MapValue::create(inner_map,{&entry,1},4)));
    const auto inner_array = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::map_field("",inner_map),{3,32,3}),1));
    struct Example { FieldDescriptor schema; Value value; uint32_t kind; };
    const Example examples[] = {{{"",ValueKind::string,3},text,DFY_V2_VALUE_STRING},
        {{"",ValueKind::structure,0,record},Value::from_structure(structure),DFY_V2_VALUE_STRUCTURE},
        {FieldDescriptor::enum_field("",enumeration),Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(enumeration,INT64_MIN))),DFY_V2_VALUE_ENUM},
        {FieldDescriptor::union_field("",choice),Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,UINT64_MAX,integer,16))),DFY_V2_VALUE_UNION},
        {FieldDescriptor::map_field("",inner_map),nested_map,DFY_V2_VALUE_MAP},
        {FieldDescriptor::array_field("",inner_array),Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(inner_array,{&nested_map,1},4))),DFY_V2_VALUE_ARRAY}};
    for (const auto& example : examples) {
        auto constraint = example.schema; constraint.nullable = true;
        const auto map_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ElementDescriptor::create(constraint,{32,512,16}),3,3));
        const MapEntry entries[] = {{"",example.value},{std::string("a\0b",3),example.value},{"nil",Value::null()}};
        const auto map_field = FieldDescriptor::map_field("items",map_type);
        const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.event",1,{&map_field,1},{64,1024,32}));
        const NamedValue input{"items",Value::from_map(std::make_shared<const MapValue>(MapValue::create(map_type,entries,128)))};
        const auto payload = std::make_shared<const StructValue>(StructValue::create(type,{&input,1},128));
        auto store = std::make_shared<dfy::events::EventStore>(4,128);
        auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store,1);
        auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue,1);
        HostServices host(16); const auto receiver = host.expose(reader); auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
        const auto& maps = service<dfy_v2_map_api_v2>(DFY_V2_MAP_INTERFACE,2);
        const auto& constraints = service<dfy_v2_constraint_api_v2>(DFY_V2_CONSTRAINT_INTERFACE,2);
        const auto& structs = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
        const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
        dfy_v2_handle request{},root{},held{},metadata{},temporary{}; dfy_test_composite_map_counts count{};
        check(reads.next(receiver,&request,nullptr) == DFY_V2_OK,"map event read failed");
        const auto event = store->accept(payload); queue->push(event); store->release(event); host.advance();
        check(reads.payload(request,&root,nullptr) == DFY_V2_OK,"map event root missing");
        check(consumer.inspect(HostServices::query,root,0,2,&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.size && !held.id && host.references() == 3,"C map budget failure leaked");
        check(consumer.inspect(HostServices::query,root,0,3,&count,&held,nullptr) == DFY_V2_OK && count.size == 3 && count.nulls == 1 && count.key_bytes == 6 &&
            count.kind == example.kind && !count.reserved && host.references() == 4,"C composite map reflection failed");
        check(structs.field(root,0,&temporary,nullptr) == DFY_V2_OK && maps.value_type(temporary,&metadata,nullptr) == DFY_V2_OK,"map metadata hold failed");
        refs.release(temporary,nullptr);
        for (unsigned depth = 0; depth < 16; ++depth) {
            dfy_v2_value_view current{}; dfy_v2_handle next{};
            check(values.read(held,&current,nullptr) == DFY_V2_OK,"map content classification failed");
            if (current.kind == DFY_V2_VALUE_MAP) check(maps.find(held,{"k",1},&next,nullptr) == DFY_V2_OK,"nested map leaf failed");
            else if (current.kind == DFY_V2_VALUE_ARRAY) check(service<dfy_v2_array_api_v2>(DFY_V2_ARRAY_INTERFACE,2).element(held,0,&next,nullptr) == DFY_V2_OK,"map array leaf failed");
            else if (current.kind == DFY_V2_VALUE_UNION) check(service<dfy_v2_union_api>(DFY_V2_UNION_INTERFACE).content(held,&next,nullptr) == DFY_V2_OK,"map union leaf failed");
            else if (current.kind == DFY_V2_VALUE_STRUCTURE) {
                check(values.structure(held,&temporary,nullptr) == DFY_V2_OK && structs.field(temporary,0,&next,nullptr) == DFY_V2_OK,"map structure leaf failed");
                refs.release(temporary,nullptr);
            } else break;
            refs.release(held,nullptr); held = next;
        }
        refs.release(root,nullptr); refs.release(request,nullptr); host.advance(); dfy_v2_value_view view{};
        check(store->references() == 1 && reader->size() == 1 && values.read(held,&view,nullptr) == DFY_V2_OK,"map leaf escaped event budget");
        check(view.kind == DFY_V2_VALUE_STRING || view.kind == DFY_V2_VALUE_UINT64 || view.kind == DFY_V2_VALUE_ENUM,"map did not resolve a leaf");
        if (view.kind == DFY_V2_VALUE_STRING) check(std::string_view(view.data.string.data,view.data.string.length) == text.as_string(),"map leaf text changed");
        if (view.kind == DFY_V2_VALUE_UINT64) check(view.data.uint64 == UINT64_MAX,"map leaf integer narrowed");
        if (view.kind == DFY_V2_VALUE_ENUM) { dfy_v2_enum_selection selected{}; check(service<dfy_v2_enum_api>(DFY_V2_ENUM_INTERFACE).read(held,&selected,nullptr) == DFY_V2_OK && selected.number.data.int64 == INT64_MIN,"map enum lost"); }
        refs.release(held,nullptr); host.advance(); dfy_v2_field_info_v2 info{};
        check(!store->references() && !reader->size() && constraints.describe(metadata,&info,nullptr) == DFY_V2_OK && info.kind == example.kind,"map metadata retained event budget");
        refs.release(metadata,nullptr); reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver,nullptr);
        const NamedValue empty_input{"items",Value::from_map(std::make_shared<const MapValue>(MapValue::create(map_type,{},0)))};
        const auto empty = host.expose(std::make_shared<const StructValue>(StructValue::create(type,{&empty_input,1},0)));
        check(consumer.inspect(HostServices::query,empty,0,0,&count,&held,nullptr) == DFY_V2_OK && !count.size && !held.id && count.kind == example.kind,"empty C map lost declaration");
        refs.release(empty,nullptr); check(!host.references() && !host.resources(),"C map resources leaked");
        {
            HostServices full(3); const auto source = full.expose(payload); auto scope = full.bind();
            check(consumer.inspect(HostServices::query,source,0,3,&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.size && !held.id && full.references() == 1,"C map partial acquisition leaked");
            refs.release(source,nullptr);
        }
    }
}
}
// 串联生产映射边界及真实 C DLL 的完整值消费。
// 步骤：验收生产服务 → 查询纯 C 消费者 → 验证复杂值与事件预算链。
int wmain(int argc, wchar_t** argv) {
    if (inspection()) return 1;
    try {
        check(argc == 2,"C composite map consumer DLL required");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap,"C map bootstrap failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_COMPOSITE_MAPS_ID,sizeof(DFY_TEST_COMPOSITE_MAPS_ID)-1},1,&table,nullptr) == DFY_V2_OK && table,"C map consumer missing");
        const auto& consumer = *static_cast<const dfy_test_composite_maps_api*>(table);
        check(consumer.header.struct_size == sizeof(consumer) && consumer.header.interface_version == 1 && consumer.inspect,"C map consumer invalid");
        consume(consumer); std::cout << "Real C composite map checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
