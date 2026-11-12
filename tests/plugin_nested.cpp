#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/map_value.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_type_api_v2) == 32 && offsetof(dfy_v2_type_api_v2, nested) == 24);
static_assert(sizeof(dfy_v2_value_api_v2) == 24 && offsetof(dfy_v2_value_api_v2, structure) == 16);
static_assert(sizeof(dfy_v2_field_info_v2) == 32 && offsetof(dfy_v2_field_info_v2, flags) == 20);
static_assert(sizeof(dfy_v2_array_type_info) == 24 && sizeof(dfy_v2_array_info) == 32 && sizeof(dfy_v2_array_api) == 32);
static_assert(sizeof(dfy_v2_map_type_info) == 32 && sizeof(dfy_v2_map_info) == 40 && sizeof(dfy_v2_map_api) == 40);
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
// 在版本化访问、递归读取或预算链违反契约时报告失败。
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
// 查询当前绑定的精确能力，不隐式接受其他版本。
template<class T>
const T& service(std::string_view name, uint32_t version) {
    const void* table = nullptr;
    check(HostServices::query({name.data(), name.size()}, version, &table, nullptr) == DFY_V2_OK && table, "service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == version, "service version mismatch");
    return *static_cast<const T*>(table);
}
// 核对 C 存在性表达和旧版本拒绝，未知索引不混同可选字段缺失。
void presence(const dfy_v2_plugin_api& bootstrap) {
    const void* table = nullptr;
    check(bootstrap.query_interface({DFY_TEST_PRESENCE_ID, sizeof(DFY_TEST_PRESENCE_ID)-1}, 1, &table, nullptr) == DFY_V2_OK, "presence consumer missing");
    const auto* consumer = static_cast<const dfy_test_presence_api*>(table);
    check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 && consumer->inspect, "invalid presence consumer");
    const FieldDescriptor fields[] = {{"value", ValueKind::string, 0}, {"missing", ValueKind::string, 0, {}, true, false},
        {"null", ValueKind::string, 0, {}, false, true}};
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.presence", 1, fields, {3,64}));
    const NamedValue input[] = {{"value", Value::from_string({},0)}, {"null", Value::null()}};
    HostServices host(8); const auto root = host.expose(std::make_shared<const StructValue>(StructValue::create(type, input, 0)));
    auto binding = host.bind(); uint32_t states = 0;
    check(consumer->inspect(HostServices::query, root, &states, nullptr) == DFY_V2_OK && states == 7 && host.references() == 1, "C presence collapsed or leaked");
    const auto& structures = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE, 2);
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE, 1);
    dfy_v2_handle field{}, descriptor{};
    check(structures.field(root, 1, &field, nullptr) == DFY_V2_NOT_FOUND && !field.id &&
        structures.field(root, 3, &field, nullptr) == DFY_V2_INVALID_ARGUMENT && !field.id, "missing and invalid index collapsed");
    check(service<dfy_v2_struct_api>(DFY_V2_STRUCT_INTERFACE, 1).field(root, 1, &field, nullptr) == DFY_V2_UNSUPPORTED && !field.id, "v1 accepted optional field");
    check(structures.field(root, 2, &field, nullptr) == DFY_V2_OK && structures.descriptor(root, &descriptor, nullptr) == DFY_V2_OK, "nullable field missing");
    dfy_v2_value_view value{}; dfy_v2_field_info info{};
    check(service<dfy_v2_value_api>(DFY_V2_VALUE_INTERFACE, 1).read(field, &value, nullptr) == DFY_V2_UNSUPPORTED && !value.kind &&
        service<dfy_v2_type_api>(DFY_V2_TYPE_INTERFACE, 1).field(descriptor, 2, &info, nullptr) == DFY_V2_UNSUPPORTED && !info.kind,
        "v1 accepted nullable semantics");
    refs.release(field, nullptr); refs.release(descriptor, nullptr); refs.release(root, nullptr);
    check(!host.references(), "presence references leaked");
}
// 以真实 C 消费数组并证明叶元素继续保持整个事件预算。
// 步骤：构造可空数组事件 → 协商并枚举 → 释放父链后读取元素 → 归还数据并关闭。
void arrays(const dfy_v2_plugin_api& bootstrap) {
    const void* table = nullptr;
    check(bootstrap.query_interface({DFY_TEST_ARRAYS_ID, sizeof(DFY_TEST_ARRAYS_ID)-1},1,&table,nullptr) == DFY_V2_OK, "array consumer missing");
    const auto* consumer = static_cast<const dfy_test_arrays_api*>(table);
    check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 && consumer->inspect, "invalid array consumer");
    auto array_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::uint64,3,0,true));
    const Value elements[] = {Value::from_uint64(1), Value::null(), Value::from_uint64(UINT64_MAX)};
    auto array = std::make_shared<const ArrayValue>(ArrayValue::create(array_type,elements,16));
    const auto field = FieldDescriptor::array_field("items",array_type);
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.array",1,{&field,1},{1,64,2}));
    const NamedValue input{"items",Value::from_array(array)};
    auto payload = std::make_shared<const StructValue>(StructValue::create(type,{&input,1},16));
    auto store = std::make_shared<dfy::events::EventStore>(4,16);
    auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store,1);
    auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue,1);
    HostServices host(16); const auto receiver = host.expose(reader); auto binding = host.bind();
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE,1);
    const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE,1);
    const auto& structures = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
    const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
    const auto& api = service<dfy_v2_array_api>(DFY_V2_ARRAY_INTERFACE,1);
    dfy_v2_handle request{}, root{}, array_ref{}, held{}, rejected{};
    check(reads.next(receiver,&request,nullptr) == DFY_V2_OK,"array read submission failed");
    const auto event = store->accept(payload); queue->push(event); store->release(event); host.advance();
    check(reads.payload(request,&root,nullptr) == DFY_V2_OK,"array root missing");
    dfy_test_array_counts count{};
    check(consumer->inspect(HostServices::query,root,0,&count,nullptr) == DFY_V2_OK && count.size == 3 && count.nulls == 1 &&
        count.last_unsigned == UINT64_MAX && host.references() == 3,"C array content or cleanup failed");
    check(structures.field(root,0,&array_ref,nullptr) == DFY_V2_OK && api.element(array_ref,2,&held,nullptr) == DFY_V2_OK,"array element hold failed");
    dfy_v2_value_view view{};
    check(values.read(array_ref,&view,nullptr) == DFY_V2_OK && view.kind == DFY_V2_VALUE_ARRAY && !view.data.uint64 && !view.data.bytes.length,
        "array classification exposed implicit data");
    check(service<dfy_v2_value_api>(DFY_V2_VALUE_INTERFACE,1).read(array_ref,&view,nullptr) == DFY_V2_UNSUPPORTED && !view.kind &&
        api.element(array_ref,3,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"array version or index boundary failed");
    refs.release(array_ref,nullptr); refs.release(root,nullptr); refs.release(request,nullptr); host.advance();
    check(store->references() == 1 && reader->size() == 1 && values.read(held,&view,nullptr) == DFY_V2_OK &&
        view.kind == DFY_V2_VALUE_UINT64 && view.data.uint64 == UINT64_MAX,"array leaf escaped event budget");
    refs.release(held,nullptr); host.advance();
    check(!store->references() && !reader->size(),"array event not reclaimed");
    reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver,nullptr);
    const auto empty = host.expose(std::make_shared<const Value>(Value::from_array(std::make_shared<const ArrayValue>(
        ArrayValue::create(std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::boolean,0)),{},0)))));
    dfy_v2_array_info info{};
    check(api.describe(empty,&info,nullptr) == DFY_V2_OK && !info.size && !info.type.max_elements &&
        api.element(empty,0,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"empty array confused with null or missing");
    refs.release(empty,nullptr); check(!host.references() && !host.resources(),"array host leaked");
}
// 通过纯 C 验证映射精确查询、借用键以及独立叶值保持的事件预算。
// 步骤：构造映射事件 → 反射并重复查键 → 检查空键和错误输出 → 释放父链并保留叶值。
void maps(const dfy_v2_plugin_api& bootstrap) {
    const void* table = nullptr;
    check(bootstrap.query_interface({DFY_TEST_MAPS_ID,sizeof(DFY_TEST_MAPS_ID)-1},1,&table,nullptr) == DFY_V2_OK,"map consumer missing");
    const auto* consumer = static_cast<const dfy_test_maps_api*>(table);
    check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 && consumer->inspect,"invalid map consumer");
    auto map_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::uint64,3,3,0,true));
    const MapEntry entries[] = {{"nil",Value::null()},{std::string("a\0b",3),Value::from_uint64(UINT64_MAX)},{"",Value::from_uint64(1)}};
    auto map = std::make_shared<const MapValue>(MapValue::create(map_type,entries,22));
    const auto field = FieldDescriptor::map_field("items",map_type);
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.map",1,{&field,1},{1,64,2}));
    const NamedValue input{"items",Value::from_map(map)};
    auto payload = std::make_shared<const StructValue>(StructValue::create(type,{&input,1},22));
    auto store = std::make_shared<dfy::events::EventStore>(4,22);
    auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store,1);
    auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue,1);
    HostServices host(16); const auto receiver = host.expose(reader); auto binding = host.bind();
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE,1);
    const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE,1);
    const auto& structures = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
    const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
    const auto& api = service<dfy_v2_map_api>(DFY_V2_MAP_INTERFACE,1);
    dfy_v2_handle request{},root{},map_ref{},held{},temporary{},rejected{};
    check(reads.next(receiver,&request,nullptr) == DFY_V2_OK,"map read submission failed");
    const auto event=store->accept(payload); queue->push(event); store->release(event); host.advance();
    check(reads.payload(request,&root,nullptr) == DFY_V2_OK,"map root missing");
    dfy_test_map_counts count{};
    check(consumer->inspect(HostServices::query,root,0,&count,nullptr) == DFY_V2_OK && count.size == 3 && count.nulls == 1 &&
        count.key_bytes == 6 && count.last_unsigned == UINT64_MAX && host.references() == 3,"C map read or cleanup failed");
    check(structures.field(root,0,&map_ref,nullptr) == DFY_V2_OK && api.find(map_ref,{"a\0b",3},&held,nullptr) == DFY_V2_OK,"embedded-zero key lost");
    dfy_v2_value_view view{}; dfy_v2_string_view key{};
    check(api.entry(map_ref,1,&key,&temporary,nullptr) == DFY_V2_OK && std::string_view(key.data,key.length) == std::string_view("a\0b",3),"key borrow changed");
    refs.release(temporary,nullptr);
    check(api.find(map_ref,{nullptr,0},&temporary,nullptr) == DFY_V2_OK && values.read(temporary,&view,nullptr) == DFY_V2_OK &&
        view.data.uint64 == 1,"empty key rejected"); refs.release(temporary,nullptr);
    check(api.find(map_ref,{"a",1},&rejected,nullptr) == DFY_V2_NOT_FOUND && !rejected.id &&
        api.find(map_ref,{nullptr,1},&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"key lookup truncated or accepted invalid view");
    check(api.entry(map_ref,3,&key,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !key.data && !key.length && !rejected.id,"invalid entry left output");
    check(service<dfy_v2_value_api>(DFY_V2_VALUE_INTERFACE,1).read(map_ref,&view,nullptr) == DFY_V2_UNSUPPORTED && !view.kind,"v1 accepted map");
    refs.release(map_ref,nullptr); refs.release(root,nullptr); refs.release(request,nullptr); host.advance();
    check(store->references() == 1 && reader->size() == 1 && values.read(held,&view,nullptr) == DFY_V2_OK &&
        view.data.uint64 == UINT64_MAX,"map value escaped event budget");
    refs.release(held,nullptr); host.advance(); reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver,nullptr);
    check(!host.references() && !host.resources() && !store->references(),"map host leaked");
    {
        HostServices full(1); const auto source=full.expose(std::make_shared<const Value>(Value::from_map(map))); auto scope=full.bind();
        key={"old",3};
        check(api.entry(source,0,&key,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !key.data && !rejected.id && full.references() == 1,
            "map entry allocation exposed partial key");
        refs.release(source,nullptr);
    }
}
}
// 以真实 C DLL 递归消费嵌套事件，保留叶字段仍需承担原事件及读取预算。
// 步骤：构造嵌套事件 → 协商版本 2 并读取 → 拒绝深度不足且回收 → 保留叶子后释放父链并检查预算。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected C nested consumer");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap, "bootstrap failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_NESTED_ID, sizeof(DFY_TEST_NESTED_ID)-1}, 1, &table, nullptr) == DFY_V2_OK, "consumer missing");
        const auto* consumer = static_cast<const dfy_test_nested_api*>(table);
        check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 && consumer->inspect,
            "invalid nested consumer");
        const FieldDescriptor text{"text", ValueKind::string, 3};
        auto leaf = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.leaf", 1, {&text,1}, {1,32}));
        const NamedValue text_value{"text", Value::from_string(std::string("A\0B",3),3)};
        auto child = std::make_shared<const StructValue>(StructValue::create(leaf, {&text_value,1}, 3));
        const FieldDescriptor fields[] = {{"left", ValueKind::structure, 0, leaf}, {"right", ValueKind::structure, 0, leaf}};
        auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.outer", 1, fields, {4,128,2}));
        const NamedValue values[] = {{"left", Value::from_structure(child)}, {"right", Value::from_structure(child)}};
        auto payload = std::make_shared<const StructValue>(StructValue::create(type, values, 6));
        auto store = std::make_shared<dfy::events::EventStore>(4, 6);
        auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store, 1);
        auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue, 1);
        HostServices host(24); const auto receiver = host.expose(reader); auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE, 1);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE, 1);
        const auto& structures = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE, 2);
        const auto& data = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE, 2);
        dfy_v2_handle request{}, root{}, field{}, structure{}, held{};
        check(reads.next(receiver, &request, nullptr) == DFY_V2_OK, "read submission failed");
        const auto event = store->accept(payload); queue->push(event); store->release(event); host.advance();
        check(reads.payload(request, &root, nullptr) == DFY_V2_OK, "root payload missing");
        dfy_test_nested_counts count{};
        check(consumer->inspect(HostServices::query, root, 2, &count, nullptr) == DFY_V2_OK && count.leaves == 2 && count.bytes == 6 &&
            host.references() == 3, "recursive C read failed or leaked");
        check(consumer->inspect(HostServices::query, root, 1, &count, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.leaves &&
            !count.bytes && host.references() == 3, "recursive failure leaked references");
        check(structures.field(root, 0, &field, nullptr) == DFY_V2_OK && data.structure(field, &structure, nullptr) == DFY_V2_OK &&
            structures.field(structure, 0, &held, nullptr) == DFY_V2_OK, "nested alias failed");
        refs.release(root, nullptr); refs.release(field, nullptr); refs.release(structure, nullptr); refs.release(request, nullptr); host.advance();
        dfy_v2_value_view view{};
        check(store->references() == 1 && reader->size() == 1 && data.read(held, &view, nullptr) == DFY_V2_OK &&
            std::string_view(view.data.string.data, view.data.string.length) == std::string_view("A\0B",3), "leaf escaped parent budget or data");
        dfy_v2_handle invalid{};
        check(data.structure(held, &invalid, nullptr) == DFY_V2_INVALID_ARGUMENT && !invalid.id, "scalar treated as structure");
        refs.release(held, nullptr); host.advance();
        check(!store->references() && !reader->size(), "leaf held finished event");
        reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver, nullptr);
        check(!host.references() && !host.resources(), "nested host leaked");
        presence(*bootstrap);
        arrays(*bootstrap);
        maps(*bootstrap);
        std::cout << "Versioned C nested checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
