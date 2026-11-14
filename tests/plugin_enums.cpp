#include <dafeiyu/host_services.hpp>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/data_abi_v2.h>
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
}
// 验证三种整数边界及无上下文调用，不依赖真实模型或插件实现。
// 步骤：验收三种极值 → 拒绝未绑定查询 → 报告结果。
int main() {
    try {
        inspect(INT64_MIN,ValueKind::int64); inspect(INT64_MAX,ValueKind::int64); inspect(UINT64_MAX,ValueKind::uint64);
        const void* table = nullptr;
        check(HostServices::query({DFY_V2_ENUM_INTERFACE,sizeof(DFY_V2_ENUM_INTERFACE)-1},1,&table,nullptr) == DFY_V2_INVALID_CONTEXT && !table,"unbound enum service accepted");
        std::cout << "C enum service checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
