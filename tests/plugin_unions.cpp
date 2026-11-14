#include <dafeiyu/host_services.hpp>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/union_value.hpp>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/map_value.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
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
// 通过真实 C DLL 读取复杂分支，并验证取得叶内容后的事件预算不能提前归还。
// 步骤：构造各类分支与数据 → 纯 C 有界反射并留存内容 → 取得叶值后释放父链 → 核对计量及失败回收。
void consume(const dfy_test_unions_api& consumer) {
    const auto text = Value::from_string(std::string("a\0b",3),3);
    const FieldDescriptor leaf_field{"text",ValueKind::string,3};
    const auto leaf_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.leaf",1,{&leaf_field,1},{1,32}));
    const NamedValue leaf_input{"text",text};
    const auto leaf = std::make_shared<const StructValue>(StructValue::create(leaf_type,{&leaf_input,1},3));
    const EnumMember enum_member{"min",INT64_MIN};
    const auto enum_type = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.enum",1,ValueKind::int64,{&enum_member,1},1,32));
    const auto enumeration = std::make_shared<const EnumValue>(EnumValue::create(enum_type,INT64_MIN));
    const auto array_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::uint64,1));
    const Value integer = Value::from_uint64(UINT64_MAX);
    const auto array = std::make_shared<const ArrayValue>(ArrayValue::create(array_type,{&integer,1},8));
    const auto map_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::string,1,1,3));
    const MapEntry entry{"k",text}; const auto map = std::make_shared<const MapValue>(MapValue::create(map_type,{&entry,1},4));
    const UnionBranch inner_branch{0,{"text",ValueKind::string,3}};
    const auto inner_type = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.inner",1,{&inner_branch,1},{1,32}));
    const auto inner = std::make_shared<const UnionValue>(UnionValue::create(inner_type,0,text,11));
    const UnionBranch branches[] = {{0,{"text",ValueKind::string,3,{},false,true}}, {1,{"number",ValueKind::uint64,0}},
        {2,FieldDescriptor::enum_field("state",enum_type)}, {3,{"record",ValueKind::structure,0,leaf_type}},
        {4,FieldDescriptor::array_field("list",array_type)}, {5,FieldDescriptor::map_field("lookup",map_type)},
        {UINT64_MAX,FieldDescriptor::union_field("nested",inner_type)}};
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.wrapper",1,branches,{16,512,2}));
    const auto field = FieldDescriptor::union_field("choice",choice);
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.root",1,{&field,1},{17,1024,3}));
    struct Example { uint64_t tag; Value value; uint32_t kind; };
    const Example cases[] = {{0,text,DFY_V2_VALUE_STRING},{0,Value::null(),DFY_V2_VALUE_NULL},{1,integer,DFY_V2_VALUE_UINT64},
        {2,Value::from_enum(enumeration),DFY_V2_VALUE_ENUM},{3,Value::from_structure(leaf),DFY_V2_VALUE_STRUCTURE},
        {4,Value::from_array(array),DFY_V2_VALUE_ARRAY},{5,Value::from_map(map),DFY_V2_VALUE_MAP},{UINT64_MAX,Value::from_union(inner),DFY_V2_VALUE_UNION}};
    for (const auto& example : cases) {
        const NamedValue input{"choice",Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,example.tag,example.value,64)))};
        const auto payload = std::make_shared<const StructValue>(StructValue::create(type,{&input,1},64));
        auto store = std::make_shared<dfy::events::EventStore>(4,64);
        auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store,1);
        auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue,1);
        HostServices host(16); const auto receiver = host.expose(reader); auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
        const auto& unions = service<dfy_v2_union_api>(DFY_V2_UNION_INTERFACE);
        const auto& structs = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
        const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
        dfy_v2_handle request{},root{},held{},next{},temporary{},description{}; dfy_test_union_counts count{};
        check(reads.next(receiver,&request,nullptr) == DFY_V2_OK,"union event read failed");
        const auto event = store->accept(payload); queue->push(event); store->release(event); host.advance();
        check(reads.payload(request,&root,nullptr) == DFY_V2_OK,"union event payload missing");
        check(consumer.inspect(HostServices::query,root,0,6,&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.branches && !held.id && host.references() == 3,"C union budget failure leaked");
        check(consumer.inspect(HostServices::query,root,0,7,&count,&held,nullptr) == DFY_V2_OK && count.branches == 7 && count.tag == example.tag &&
            count.content_kind == example.kind && count.kinds == 7848 && host.references() == 4,"C union branch reflection failed");
        check(structs.field(root,0,&temporary,nullptr) == DFY_V2_OK && unions.descriptor(temporary,&description,nullptr) == DFY_V2_OK,"union metadata hold failed");
        refs.release(temporary,nullptr);
        if (example.kind == DFY_V2_VALUE_UNION) check(unions.content(held,&next,nullptr) == DFY_V2_OK,"nested union content failed");
        if (example.kind == DFY_V2_VALUE_ARRAY) check(service<dfy_v2_array_api>(DFY_V2_ARRAY_INTERFACE).element(held,0,&next,nullptr) == DFY_V2_OK,"union array leaf failed");
        if (example.kind == DFY_V2_VALUE_MAP) check(service<dfy_v2_map_api>(DFY_V2_MAP_INTERFACE).find(held,{"k",1},&next,nullptr) == DFY_V2_OK,"union map leaf failed");
        if (example.kind == DFY_V2_VALUE_STRUCTURE) {
            check(values.structure(held,&temporary,nullptr) == DFY_V2_OK && structs.field(temporary,0,&next,nullptr) == DFY_V2_OK,"union structure leaf failed");
            refs.release(temporary,nullptr);
        }
        if (next.id) { refs.release(held,nullptr); held = next; }
        refs.release(root,nullptr); refs.release(request,nullptr); host.advance();
        dfy_v2_value_view view{};
        check(store->references() == 1 && reader->size() == 1 && values.read(held,&view,nullptr) == DFY_V2_OK,"union leaf escaped event budget");
        if (view.kind == DFY_V2_VALUE_STRING) check(std::string_view(view.data.string.data,view.data.string.length) == text.as_string(),"union leaf text changed");
        if (view.kind == DFY_V2_VALUE_UINT64) check(view.data.uint64 == UINT64_MAX,"union leaf integer narrowed");
        if (view.kind == DFY_V2_VALUE_ENUM) { dfy_v2_enum_selection selected{}; check(service<dfy_v2_enum_api>(DFY_V2_ENUM_INTERFACE).read(held,&selected,nullptr) == DFY_V2_OK && selected.number.data.int64 == INT64_MIN,"union enum lost"); }
        refs.release(held,nullptr); host.advance(); dfy_v2_union_info info{};
        check(!store->references() && !reader->size() && unions.describe(description,&info,nullptr) == DFY_V2_OK,"union metadata retained event budget");
        refs.release(description,nullptr); reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver,nullptr);
        check(!host.references() && !host.resources(),"C union resources leaked");
        {
            HostServices full(3); const auto source = full.expose(payload); auto scope = full.bind();
            check(consumer.inspect(HostServices::query,source,0,7,&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.branches && !held.id && full.references() == 1,"C union partial acquisition leaked");
            refs.release(source,nullptr);
        }
    }
}
}
// 通过生产服务与真实 C DLL 验证联合反射、内容和事件预算链。
// 步骤：核对生产边界 → 查询纯 C 消费者 → 验收全部复杂分支及回收。
int wmain(int argc, wchar_t** argv) {
    try { inspect(0,Value::from_string(std::string("a\0b",3),3)); inspect(0,Value::null()); inspect(UINT64_MAX,Value::from_uint64(UINT64_MAX));
        check(argc == 2,"C union consumer DLL required");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap,"C union bootstrap failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_UNIONS_ID,sizeof(DFY_TEST_UNIONS_ID)-1},1,&table,nullptr) == DFY_V2_OK && table,"C union consumer missing");
        const auto& consumer = *static_cast<const dfy_test_unions_api*>(table);
        check(consumer.header.struct_size == sizeof(consumer) && consumer.header.interface_version == 1 && consumer.inspect,"C union consumer invalid");
        consume(consumer);
        std::cout << "C union service checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
