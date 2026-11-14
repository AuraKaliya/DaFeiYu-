#include <dafeiyu/host_services.hpp>
#include <dafeiyu/array_value.hpp>
#include <dafeiyu/data_abi_v2.h>
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
int main() {
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
