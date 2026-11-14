#include <dafeiyu/host_services.hpp>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_descriptor.hpp>
#include <dafeiyu/map_value.hpp>
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_constraint_api) == 48 && offsetof(dfy_v2_constraint_api,map) == 40);
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
// 在内容约束反射、版本或寿命违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 查询精确版本与布局，拒绝不匹配的函数表。
// 步骤：查询能力 → 核对公共前缀 → 返回对应类型。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"constraint service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1,"constraint table invalid");
    return *static_cast<const T*>(table);
}
}
// 验证独立字段约束可反射全部已实现内容，并保持描述与数据责任分离。
// 步骤：建立各类字段 → 比较原接口和独立约束 → 拒绝错误与预算不足 → 释放父描述后重新读取并回收。
int main() {
    try {
        const auto child = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.child",1,{}, {0,32}));
        const EnumMember member{"on",std::int64_t{1}};
        const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.enum",1,ValueKind::int64,{&member,1},1,32));
        const UnionBranch branch{0,{"one",ValueKind::boolean,0}};
        const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.union",1,{&branch,1},{1,32}));
        const FieldDescriptor fields[] = {{"flag",ValueKind::boolean,0},{"text",ValueKind::string,4,{},true,true},
            {"child",ValueKind::structure,0,child},FieldDescriptor::enum_field("state",enumeration),
            FieldDescriptor::array_field("list",std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::int64,2))),
            FieldDescriptor::map_field("lookup",std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::string,2,2,4,true))),
            FieldDescriptor::union_field("future",choice)};
        auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.fields",1,fields,{16,256,2}));
        const std::weak_ptr<const StructDescriptor> lifetime = type;
        HostServices host(4); const auto root = host.expose(type); auto binding = host.bind();
        const auto& api = service<dfy_v2_constraint_api>(DFY_V2_CONSTRAINT_INTERFACE);
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        dfy_v2_handle constraint{},named{},rejected{}; dfy_v2_field_info_v2 info{};
        for (uint64_t i = 0; i < 7; ++i) {
            check(api.field(root,i,&constraint,nullptr) == DFY_V2_OK && api.describe(constraint,&info,nullptr) == DFY_V2_OK &&
                std::string_view(info.name.data,info.name.length) == fields[i].name,"constraint field metadata wrong");
            if (i == 1) check(info.flags == (DFY_V2_FIELD_OPTIONAL | DFY_V2_FIELD_NULLABLE) && info.max_bytes == 4,"constraint flags lost");
            if (i == 2 || i == 3 || i == 6) {
                check(api.named_type(constraint,&named,nullptr) == DFY_V2_OK,"named constraint missing");
                if (i == 2) { dfy_v2_type_info nested{}; check(service<dfy_v2_type_api>(DFY_V2_TYPE_INTERFACE).describe(named,&nested,nullptr) == DFY_V2_OK && nested.field_count == 0,"structure constraint changed"); }
                else if (i == 3) { dfy_v2_enum_info nested{}; check(service<dfy_v2_enum_api>(DFY_V2_ENUM_INTERFACE).describe(named,&nested,nullptr) == DFY_V2_OK && nested.member_count == 1,"enum constraint changed"); }
                else { dfy_v2_union_info nested{}; check(service<dfy_v2_union_api>(DFY_V2_UNION_INTERFACE).describe(named,&nested,nullptr) == DFY_V2_OK && nested.branch_count == 1,"union constraint changed"); }
                refs.release(named,nullptr);
            }
            if (i == 4) { dfy_v2_array_type_info array{}; check(api.array(constraint,&array,nullptr) == DFY_V2_OK && array.element_kind == DFY_V2_VALUE_INT64 && array.max_elements == 2,"array constraint changed"); }
            if (i == 5) { dfy_v2_map_type_info map{}; check(api.map(constraint,&map,nullptr) == DFY_V2_OK && map.value_kind == DFY_V2_VALUE_STRING && map.max_entries == 2 &&
                map.max_key_bytes == 2 && map.max_value_bytes == 4 && map.value_flags == DFY_V2_ELEMENT_NULLABLE,"map constraint changed"); }
            refs.release(constraint,nullptr);
        }
        check(api.field(root,7,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id &&
            api.field(root,UINT64_MAX,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"invalid constraint accepted");
        check(api.describe(root,&info,nullptr) == DFY_V2_INVALID_ARGUMENT && !info.name.data && api.describe(root,nullptr,nullptr) == DFY_V2_INVALID_ARGUMENT,"wrong constraint input accepted");
        const void* unavailable = &api;
        check(HostServices::query({DFY_V2_CONSTRAINT_INTERFACE,sizeof(DFY_V2_CONSTRAINT_INTERFACE)-1},2,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable,"constraint wrong version accepted");
        check(api.field(root,1,&constraint,nullptr) == DFY_V2_OK,"constraint hold failed");
        dfy_v2_array_type_info array{}; dfy_v2_map_type_info map{};
        check(api.array(constraint,&array,nullptr) == DFY_V2_INVALID_ARGUMENT && !array.element_kind &&
            api.map(constraint,&map,nullptr) == DFY_V2_INVALID_ARGUMENT && !map.value_kind && api.named_type(constraint,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"scalar constraint became composite");
        {
            HostServices full(1); const auto full_root = full.expose(type); auto scope = full.bind();
            check(api.field(full_root,0,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && full.references() == 1,"constraint capacity failure leaked");
            check(api.describe(constraint,&info,nullptr) == DFY_V2_INVALID_HANDLE && !info.kind,"cross-domain constraint accepted");
            refs.release(full_root,nullptr);
        }
        {
            const auto elements = ElementDescriptor::create(FieldDescriptor::enum_field("",enumeration,false,true),{2,32});
            const auto array_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(elements,1));
            const Value null = Value::null();
            HostServices limited(4); auto scope = limited.bind();
            const auto source = limited.expose(std::make_shared<const Value>(Value::from_array(
                std::make_shared<const ArrayValue>(ArrayValue::create(array_type,{&null,1},0)))));
            const auto array_field = FieldDescriptor::array_field("list",array_type);
            const auto schema = limited.expose(std::make_shared<const StructDescriptor>(StructDescriptor::create("test.composite",1,{&array_field,1},{3,64,2})));
            const auto& arrays = service<dfy_v2_array_api>(DFY_V2_ARRAY_INTERFACE); dfy_v2_array_info actual{};
            check(arrays.describe(source,&actual,nullptr) == DFY_V2_UNSUPPORTED && !actual.type.element_kind &&
                arrays.element(source,0,&rejected,nullptr) == DFY_V2_UNSUPPORTED && !rejected.id,"legacy arrays accepted composite declaration through null");
            check(api.field(schema,0,&named,nullptr) == DFY_V2_OK && api.array(named,&array,nullptr) == DFY_V2_UNSUPPORTED && !array.element_kind,
                "legacy constraint accepted composite array");
            refs.release(named,nullptr); refs.release(schema,nullptr); refs.release(source,nullptr);
            check(!limited.references(),"legacy array rejection leaked");
        }
        type.reset(); refs.release(root,nullptr);
        check(!lifetime.expired() && api.describe(constraint,&info,nullptr) == DFY_V2_OK && info.max_bytes == 4,"constraint lost parent metadata");
        refs.release(constraint,nullptr);
        check(lifetime.expired() && !host.references() && api.describe(constraint,&info,nullptr) == DFY_V2_INVALID_HANDLE && !info.kind,"constraint not reclaimed");
        std::cout << "Independent constraint checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
