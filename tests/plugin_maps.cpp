#include <dafeiyu/host_services.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/data_abi_v2.h>
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
int main() {
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
