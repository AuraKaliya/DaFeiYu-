#include <dafeiyu/host_services.hpp>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/data_abi_v2.h>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_enum_info) == 32 && offsetof(dfy_v2_enum_info, member_count) == 24);
static_assert(sizeof(dfy_v2_enum_member) == 40 && offsetof(dfy_v2_enum_member, number) == 16);
static_assert(sizeof(dfy_v2_enum_selection) == 32 && offsetof(dfy_v2_enum_selection, number) == 8);
static_assert(sizeof(dfy_v2_enum_api) == 48 && offsetof(dfy_v2_enum_api, read) == 40);
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
// 在 C 枚举服务的精度、持有或错误边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 精确查询服务布局，避免版本或大小不匹配时解引用函数表。
// 步骤：查询能力 → 校验公共前缀 → 返回精确类型表。
template<class T>
const T& service(std::string_view name, uint32_t version = 1) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},version,&table,nullptr) == DFY_V2_OK && table,"enum service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == version,"enum table mismatch");
    return *static_cast<const T*>(table);
}
// 验证 C 反射与值选择、旧能力拒绝和描述独立寿命。
// 步骤：构造极值枚举字段 → 查询声明和数据 → 核对精度及失败输出 → 释放数据后重新借用描述。
void inspect(EnumNumber number, ValueKind kind) {
    const EnumMember member{"edge", number};
    auto definition = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.enum",UINT32_MAX,kind,{&member,1},1,13));
    const auto field = FieldDescriptor::enum_field("value",definition);
    auto structure = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.root",1,{&field,1},{2,27}));
    const NamedValue input{"value",Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(definition,number)))};
    auto payload = std::make_shared<const StructValue>(StructValue::create(structure,{&input,1},8));
    const std::weak_ptr<const StructValue> lifetime = payload;
    HostServices host(8); const auto root = host.expose(payload); payload.reset();
    auto binding = host.bind();
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
    const auto& enums = service<dfy_v2_enum_api>(DFY_V2_ENUM_INTERFACE);
    const auto& structs = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE,2);
    const auto& values = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE,2);
    dfy_v2_handle type{}, declared{}, value{}, actual{}, rejected{};
    check(structs.descriptor(root,&type,nullptr) == DFY_V2_OK && enums.field_type(type,0,&declared,nullptr) == DFY_V2_OK &&
        structs.field(root,0,&value,nullptr) == DFY_V2_OK && enums.descriptor(value,&actual,nullptr) == DFY_V2_OK,"enum references failed");
    dfy_v2_enum_info info{}; dfy_v2_enum_member entry{}; dfy_v2_enum_selection selection{}; dfy_v2_value_view view{};
    check(enums.describe(declared,&info,nullptr) == DFY_V2_OK && info.contract_version == UINT32_MAX && info.member_count == 1 &&
        enums.member(actual,0,&entry,nullptr) == DFY_V2_OK && enums.read(value,&selection,nullptr) == DFY_V2_OK,"enum reflection failed");
    check(info.underlying_kind == selection.number.kind && selection.index == 0 && !selection.number.reserved &&
        std::string_view(entry.name.data,entry.name.length) == "edge","enum metadata changed");
    if (kind == ValueKind::int64) check(selection.number.data.int64 == std::get<std::int64_t>(number) &&
        entry.number.data.int64 == selection.number.data.int64,"signed enum narrowed");
    else check(selection.number.data.uint64 == UINT64_MAX && entry.number.data.uint64 == UINT64_MAX,"unsigned enum narrowed");
    check(values.read(value,&view,nullptr) == DFY_V2_OK && view.kind == DFY_V2_VALUE_ENUM && !view.data.uint64 && !view.data.bytes.length,"enum category not opaque");
    check(service<dfy_v2_value_api>(DFY_V2_VALUE_INTERFACE).read(value,&view,nullptr) == DFY_V2_UNSUPPORTED && !view.kind &&
        service<dfy_v2_struct_api>(DFY_V2_STRUCT_INTERFACE).field(root,0,&rejected,nullptr) == DFY_V2_UNSUPPORTED && !rejected.id,"v1 accepted enum");
    dfy_v2_field_info old{};
    check(service<dfy_v2_type_api>(DFY_V2_TYPE_INTERFACE).field(type,0,&old,nullptr) == DFY_V2_UNSUPPORTED && !old.kind,"v1 reflected enum");
    check(enums.member(actual,UINT64_MAX,&entry,nullptr) == DFY_V2_INVALID_ARGUMENT && !entry.name.data && !entry.number.kind &&
        enums.read(root,&selection,nullptr) == DFY_V2_INVALID_ARGUMENT && !selection.number.kind,"enum failure left output");
    check(enums.field_type(type,1,&rejected,nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id &&
        enums.describe(type,&info,nullptr) == DFY_V2_INVALID_ARGUMENT && !info.name.data && enums.read(value,nullptr,nullptr) == DFY_V2_INVALID_ARGUMENT,"enum invalid input accepted");
    const void* unavailable = &enums;
    check(HostServices::query({DFY_V2_ENUM_INTERFACE,sizeof(DFY_V2_ENUM_INTERFACE)-1},2,&unavailable,nullptr) == DFY_V2_UNSUPPORTED && !unavailable,"enum wrong version accepted");
    refs.release(root,nullptr); refs.release(type,nullptr); refs.release(value,nullptr);
    check(lifetime.expired() && enums.describe(actual,&info,nullptr) == DFY_V2_OK && info.member_count == 1,"enum descriptor retained payload or lost identity");
    refs.release(actual,nullptr); refs.release(declared,nullptr);
    check(enums.describe(actual,&info,nullptr) == DFY_V2_INVALID_HANDLE && !info.member_count && !host.references(),"stale enum reference accepted");
    {
        HostServices full(1); const auto source = full.expose(std::make_shared<const Value>(input.value)); auto scope = full.bind();
        check(enums.descriptor(source,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && full.references() == 1,"enum capacity failure leaked");
        refs.release(source,nullptr);
    }
}
// 验证真实 C DLL 有界反射未知成员，叶引用持续保持事件及读请求预算。
// 步骤：构造整数边界事件 → 由纯 C 消费并留存字段 → 释放父链检查计量 → 归还数据并确认描述独立。
void consume(const dfy_test_enums_api& consumer, EnumNumber number, ValueKind kind) {
    const std::vector<EnumMember> members = kind == ValueKind::int64 ?
        std::vector<EnumMember>{{"min",INT64_MIN},{"zero",std::int64_t{0}},{"max",INT64_MAX}} :
        std::vector<EnumMember>{{"zero",std::uint64_t{0}},{"max",UINT64_MAX}};
    auto definition = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.event_enum",1,kind,members,3,32));
    const auto field = FieldDescriptor::enum_field("value",definition);
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.event",1,{&field,1},{4,64}));
    const NamedValue input{"value",Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(definition,number)))};
    auto payload = std::make_shared<const StructValue>(StructValue::create(type,{&input,1},8));
    auto store = std::make_shared<dfy::events::EventStore>(2,8);
    auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store,1);
    auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue,1);
    HostServices host(7); const auto receiver = host.expose(reader); auto binding = host.bind();
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
    const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
    const auto& enums = service<dfy_v2_enum_api>(DFY_V2_ENUM_INTERFACE);
    dfy_v2_handle request{},root{},held{},description{}; dfy_test_enum_counts count{};
    check(reads.next(receiver,&request,nullptr) == DFY_V2_OK,"enum read submission failed");
    const auto event = store->accept(payload); queue->push(event); store->release(event); host.advance();
    check(reads.payload(request,&root,nullptr) == DFY_V2_OK,"enum payload missing");
    check(consumer.inspect(HostServices::query,root,0,members.size()-1,&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED &&
        !count.members && !count.selection.number.kind && !held.id && host.references() == 3,"C enum budget failure leaked");
    check(consumer.inspect(HostServices::query,root,0,members.size(),&count,&held,nullptr) == DFY_V2_OK &&
        count.members == members.size() && count.selection.index == definition->index_of(number) && host.references() == 4,"C enum reflection or cleanup failed");
    check(enums.descriptor(held,&description,nullptr) == DFY_V2_OK,"enum independent descriptor failed");

    refs.release(root,nullptr); refs.release(request,nullptr); host.advance();
    dfy_v2_enum_selection selection{};
    check(store->references() == 1 && reader->size() == 1 && enums.read(held,&selection,nullptr) == DFY_V2_OK,"enum leaf escaped event budget");
    if (kind == ValueKind::int64) check(selection.number.data.int64 == std::get<std::int64_t>(number),"C signed enum changed");
    else check(selection.number.data.uint64 == UINT64_MAX,"C unsigned enum narrowed");
    refs.release(held,nullptr); host.advance();
    dfy_v2_enum_info info{};
    check(!store->references() && !reader->size() && enums.describe(description,&info,nullptr) == DFY_V2_OK,"enum descriptor retained event budget");
    refs.release(description,nullptr); reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver,nullptr);
    check(!host.references() && !host.resources(),"C enum lifecycle leaked");
    {
        HostServices full(3); const auto source = full.expose(payload); auto scope = full.bind();
        check(consumer.inspect(HostServices::query,source,0,members.size(),&count,&held,nullptr) == DFY_V2_RESOURCE_EXHAUSTED &&
            !count.members && !held.id && full.references() == 1,"C enum partial acquisition leaked");
        refs.release(source,nullptr);
    }
}
}
// 通过生产服务与真实 C DLL 验证枚举精度、预算及借用边界。
// 步骤：验收生产服务边界 → 查询纯 C 消费者 → 验证三种极值事件链 → 拒绝未绑定查询。
int wmain(int argc, wchar_t** argv) {
    try {
        inspect(INT64_MIN,ValueKind::int64); inspect(INT64_MAX,ValueKind::int64); inspect(UINT64_MAX,ValueKind::uint64);
        check(argc == 2,"C enum consumer DLL required");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap,"C enum bootstrap failed");
        const void* consumer_table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_ENUMS_ID,sizeof(DFY_TEST_ENUMS_ID)-1},1,&consumer_table,nullptr) == DFY_V2_OK && consumer_table,"C enum consumer missing");
        const auto& consumer = *static_cast<const dfy_test_enums_api*>(consumer_table);
        check(consumer.header.struct_size == sizeof(consumer) && consumer.header.interface_version == 1 && consumer.inspect,"C enum consumer invalid");
        consume(consumer,INT64_MIN,ValueKind::int64); consume(consumer,INT64_MAX,ValueKind::int64); consume(consumer,UINT64_MAX,ValueKind::uint64);
        const void* table = nullptr;
        check(HostServices::query({DFY_V2_ENUM_INTERFACE,sizeof(DFY_V2_ENUM_INTERFACE)-1},1,&table,nullptr) == DFY_V2_INVALID_CONTEXT && !table,"unbound enum service accepted");
        std::cout << "C enum service checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
