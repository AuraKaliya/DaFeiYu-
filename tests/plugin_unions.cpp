#include <dafeiyu/host_services.hpp>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/union_value.hpp>
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_union_info) == 32 && offsetof(dfy_v2_union_info,branch_count) == 24);
static_assert(sizeof(dfy_v2_union_selection) == 16 && offsetof(dfy_v2_union_selection,tag) == 8);
static_assert(sizeof(dfy_v2_union_api) == 48 && offsetof(dfy_v2_union_api,content) == 40);
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
// 在联合 C 边界、反射或数据持有违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 精确查询能力版本与布局，拒绝隐式兼容其他函数表。
// 步骤：查询服务 → 核对公共头 → 返回相应只读表。
template<class T>
const T& service(std::string_view name, uint32_t version = 1) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},version,&table,nullptr) == DFY_V2_OK && table,"union service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == version,"union service layout wrong");
    return *static_cast<const T*>(table);
}
// 验证联合的全部分支约束及选中内容，元数据与数据持有分别回收。
// 步骤：构造联合事件 → 反射分支并读取选择 → 验证旧版本及错误输出 → 释放父链后继续读取叶内容。
void inspect(std::uint64_t tag, const Value& contents) {
    const FieldDescriptor flag{"flag",ValueKind::boolean,0};
    const auto child = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.leaf",1,{&flag,1},{1,32}));
    const UnionBranch branches[] = {{0,{"text",ValueKind::string,3,{},false,true}}, {UINT64_MAX,{"number",ValueKind::uint64,0}},
        {3,{"record",ValueKind::structure,0,child}}};
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.choice",UINT32_MAX,branches,{4,128,2}));
    const auto field = FieldDescriptor::union_field("choice",choice);
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.root",1,{&field,1},{5,256,3}));
    const NamedValue input{"choice",Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,tag,contents,64)))};
    auto payload = std::make_shared<const StructValue>(StructValue::create(type,{&input,1},64));
    const std::weak_ptr<const StructValue> lifetime = payload;
    HostServices host(12); const auto root = host.expose(payload); payload.reset(); auto binding = host.bind();
    const auto& unions = service<dfy_v2_union_api>(DFY_V2_UNION_INTERFACE);
    const auto& constraints = service<dfy_v2_constraint_api>(DFY_V2_CONSTRAINT_INTERFACE);
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
    const auto& structs = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
    const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
    dfy_v2_handle value{},actual{},branch{},held{},named{},rejected{};
    check(structs.field(root,0,&value,nullptr) == DFY_V2_OK && unions.descriptor(value,&actual,nullptr) == DFY_V2_OK,"union field or descriptor failed");
    dfy_v2_union_info info{}; dfy_v2_union_selection selection{}; dfy_v2_field_info_v2 declared{}; uint64_t branch_tag = 99;
    check(unions.describe(actual,&info,nullptr) == DFY_V2_OK && info.contract_version == UINT32_MAX && !info.reserved && info.branch_count == 3 &&
        unions.read(value,&selection,nullptr) == DFY_V2_OK && selection.tag == tag && selection.index == choice->index_of(tag),"union selection changed");
    for (uint64_t i = 0; i < info.branch_count; ++i) {
        check(unions.branch(actual,i,&branch_tag,&branch,nullptr) == DFY_V2_OK && branch_tag == branches[i].tag &&
            constraints.describe(branch,&declared,nullptr) == DFY_V2_OK && std::string_view(declared.name.data,declared.name.length) == branches[i].value.name,"union branch reflection failed");
        if (i == 0) check(declared.flags == DFY_V2_FIELD_NULLABLE && declared.max_bytes == 3,"union branch null or byte constraint lost");
        if (i == 2) {
            check(constraints.named_type(branch,&named,nullptr) == DFY_V2_OK,"union structured branch schema missing");
            dfy_v2_type_info nested{};
            check(service<dfy_v2_type_api>(DFY_V2_TYPE_INTERFACE).describe(named,&nested,nullptr) == DFY_V2_OK && nested.field_count == 1,"union structured schema changed");
            refs.release(named,nullptr);
        }
        refs.release(branch,nullptr);
    }
    dfy_v2_value_view view{};
    check(values.read(value,&view,nullptr) == DFY_V2_OK && view.kind == DFY_V2_VALUE_UNION && !view.data.uint64 && !view.data.bytes.length &&
        unions.content(value,&held,nullptr) == DFY_V2_OK,"union category or content missing");
    check(service<dfy_v2_value_api>(DFY_V2_VALUE_INTERFACE).read(value,&view,nullptr) == DFY_V2_UNSUPPORTED && !view.kind &&
        service<dfy_v2_struct_api>(DFY_V2_STRUCT_INTERFACE).field(root,0,&rejected,nullptr) == DFY_V2_UNSUPPORTED && !rejected.id,"v1 accepted union");
    check(unions.branch(actual,UINT64_MAX,&branch_tag,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !branch_tag && !rejected.id &&
        unions.content(root,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id,"union invalid output not cleared");
    check(unions.read(held,&selection,nullptr) == DFY_V2_INVALID_ARGUMENT && !selection.index && !selection.tag &&
        unions.read(value,nullptr,nullptr) == DFY_V2_INVALID_ARGUMENT,"union wrong content accepted");
    const void* unavailable = &unions;
    check(HostServices::query({DFY_V2_UNION_INTERFACE,sizeof(DFY_V2_UNION_INTERFACE)-1},2,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable,"union version accepted");
    std::vector<dfy_v2_handle> filled;
    while (host.references() < 12) { dfy_v2_handle copy{}; check(refs.retain(actual,&copy,nullptr) == DFY_V2_OK,"capacity setup failed"); filled.push_back(copy); }
    branch_tag = 99;
    check(unions.branch(actual,1,&branch_tag,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !branch_tag && !rejected.id && host.references() == 12,"union branch failure partially committed");
    check(unions.content(value,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id,"union content failure leaked");
    for (const auto copy : filled) refs.release(copy,nullptr);

    refs.release(root,nullptr); refs.release(value,nullptr);
    check(!lifetime.expired() && values.read(held,&view,nullptr) == DFY_V2_OK,"union content lost parent ownership");
    if (contents.kind() == ValueKind::string) check(view.kind == DFY_V2_VALUE_STRING && std::string_view(view.data.string.data,view.data.string.length) == contents.as_string(),"union text changed");
    else if (contents.kind() == ValueKind::uint64) check(view.kind == DFY_V2_VALUE_UINT64 && view.data.uint64 == UINT64_MAX,"union integer narrowed");
    else check(view.kind == DFY_V2_VALUE_NULL,"union null content lost");
    refs.release(held,nullptr);
    check(lifetime.expired() && unions.describe(actual,&info,nullptr) == DFY_V2_OK,"union descriptor retained payload");
    refs.release(actual,nullptr);
    check(!host.references() && unions.describe(actual,&info,nullptr) == DFY_V2_INVALID_HANDLE && !info.name.data,"union reference leaked");
}
}
// 验证文本、显式 null 和最大标签／整数选择的生产 C 访问。
int main() {
    try { inspect(0,Value::from_string(std::string("a\0b",3),3)); inspect(0,Value::null()); inspect(UINT64_MAX,Value::from_uint64(UINT64_MAX));
        std::cout << "C union service checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
