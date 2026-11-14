#include <dafeiyu/plugin_instance.hpp>
#include <windows.h>
#pragma pack(push, 1)
#include "../plugins/examples/observer/observer_api.h"
struct PackingProbe { char tag; uint64_t value; };
#pragma pack(pop)
#include <cmath>
#include <iostream>
#include <dafeiyu/enum_value.hpp>
#include <dafeiyu/union_value.hpp>
#include <dafeiyu/group_abi_v2.h>
#include <dafeiyu/data_abi_v2.h>
#include <fstream>
#include <dafeiyu/map_value.hpp>
static_assert(sizeof(PackingProbe) == 9 && sizeof(dfy_observer_stats) == 112 && alignof(dfy_observer_stats) == 8);
namespace {
using namespace dfy::plugins;
using namespace dfy::runtime;
// 在独立观察插件的行为或回收不符合预期时报告失败。
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
// 在活动保护内复制指定示例能力，调用方必须继续保持代码凭证。
// 步骤：查询引导 → 精确查询能力 → 核对布局并复制。
template<class T>
T ability(dfy::platform::CodeLease& code, std::string_view name) {
    auto call = code.enter();
    const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
    const dfy_v2_plugin_api* bootstrap = nullptr;
    check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap && bootstrap->struct_size == sizeof(*bootstrap) &&
        bootstrap->abi_version == 2 && bootstrap->architecture == DFY_V2_ARCH_X64 && !bootstrap->reserved && bootstrap->query_interface,
        "observer query failed");
    const void* table = nullptr;
    check(bootstrap->query_interface({name.data(), name.size()}, 1, &table, nullptr) == DFY_V2_OK && table, "observer capability missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1, "invalid capability table");
    return *static_cast<const T*>(table);
}
// 在受控代码持有下复制统计，不让调用表或地址逃逸。
dfy_observer_stats statistics(dfy::platform::CodeLease& code) {
    const auto api = ability<dfy_observer_stats_api>(code, DFY_OBSERVER_STATS_INTERFACE);
    auto call = code.enter();
    dfy_observer_stats result{};
    check(api.read && api.read(&result, nullptr) == DFY_V2_OK, "statistics read failed");
    return result;
}
// 临时保留实例模块以读取统计，返回前归还该外部代码持有。
dfy_observer_stats statistics(PluginInstance& instance) { auto code = instance.retain_code(); return statistics(code); }
// 构造包含六种基础字段的宿主结构，名称未编译进观察 DLL。
// 步骤：定义字段描述 → 构造边界值 → 保存不可变宿主载荷。
std::shared_ptr<const StructValue> event_value() {
    const FieldDescriptor fields[] = {{"flag", ValueKind::boolean, 0}, {"signed", ValueKind::int64, 0},
        {"unsigned", ValueKind::uint64, 0}, {"ratio", ValueKind::float64, 0}, {"text", ValueKind::string, 8}, {"blob", ValueKind::bytes, 3}};
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.dynamic", 1, fields, {6, 128}));
    const uint8_t bytes[]{0, 255, 128};
    const NamedValue values[] = {{"flag", Value::from_bool(true)}, {"signed", Value::from_int64(INT64_MIN)},
        {"unsigned", Value::from_uint64(UINT64_MAX)}, {"ratio", Value::from_float64(-0.0)},
        {"text", Value::from_string(std::string("中文\0x", 8), 8)}, {"blob", Value::from_bytes(bytes, 3)}};
    return std::make_shared<const StructValue>(StructValue::create(type, values, 36));
}
// 在有限次数内推进事件及通知，等待下次事件不占执行器线程。
void drain(PluginInstance& instance, FifoExecutor& executor) {
    for (unsigned i = 0; i < 32; ++i) {
        instance.advance(executor);
        const auto result = executor.run_one();
        check(result != RunResult::failed, "observer callback failed");
        if (result == RunResult::idle) return;
    }
    throw std::runtime_error("observer did not yield");
}
// 构造含嵌套结构、可空容器、缺失字段与显式空值的事件。
// 步骤：创建子结构 → 创建标量容器 → 组装根字段并核定预算。
std::shared_ptr<const StructValue> composite_value() {
    const FieldDescriptor child_fields[] = {{"text", ValueKind::string, 3}, {"absent", ValueKind::uint64, 0, {}, true}};
    const auto child_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.child", 1, child_fields, {2, 128}));
    const NamedValue child_values[] = {{"text", Value::from_string(std::string("A\0B", 3), 3)}};
    const auto child = std::make_shared<const StructValue>(StructValue::create(child_type, child_values, 3));

    const auto array_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::uint64, 3, 0, true));
    const Value elements[] = {Value::from_uint64(1), Value::null(), Value::from_uint64(UINT64_MAX)};
    const auto array = std::make_shared<const ArrayValue>(ArrayValue::create(array_type, elements, 16));
    const auto map_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::string, 2, 1, 3, true));
    const MapEntry entries[] = {{"x", Value::from_string("字", 3)}, {"", Value::null()}};
    const auto map = std::make_shared<const MapValue>(MapValue::create(map_type, entries, 4));

    const FieldDescriptor fields[] = {{"nested", ValueKind::structure, 0, child_type}, FieldDescriptor::array_field("array", array_type),
        FieldDescriptor::map_field("map", map_type), {"nil", ValueKind::string, 0, {}, false, true}, {"missing", ValueKind::bytes, 0, {}, true}};
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.composite", 1, fields, {9, 256, 2}));
    const NamedValue values[] = {{"nested", Value::from_structure(child)}, {"array", Value::from_array(array)},
        {"map", Value::from_map(map)}, {"nil", Value::null()}};
    return std::make_shared<const StructValue>(StructValue::create(type, values, 23, 13));
}
// 验证实际 DLL 消费组合分类，精确预算可用而超限关闭自身。
// 步骤：消费组合事件 → 构造数量、稀疏字段及深度边界 → 验证共享访问预算 → 核对回收和卸载。
void composites(const Manifest& manifest, const std::filesystem::path& data) {
    auto store = std::make_shared<dfy::events::EventStore>(8, 8192);
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(store, 2, 1);
    FifoExecutor executor(2);
    {
        PluginInstance instance(manifest, data, channel);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK, "composite observer setup failed");
        channel->publish(composite_value()); drain(instance, executor);
        const auto observed = statistics(instance);
        check(observed.events == 1 && observed.fields == 5 && observed.bytes == 7 && observed.kinds == 1960 &&
            observed.last_uint64 == UINT64_MAX && !observed.failures && !store->references(), "composite event lost semantics or retained data");
        check(instance.shutdown() == ShutdownResult::closed && !GetModuleHandleW(manifest.entry.c_str()), "composite observer remains loaded");
    }

    std::vector<FieldDescriptor> wide_fields;
    std::vector<NamedValue> wide_values;
    for (unsigned i = 0; i < 4100; ++i) {
        auto name = "f" + std::to_string(i);
        wide_fields.push_back({name, ValueKind::boolean, 0}); wide_values.push_back({std::move(name), Value::from_bool(true)});
    }
    const auto wide_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.wide", 1, wide_fields, {4100, 65536}));
    const auto wide = std::make_shared<const StructValue>(StructValue::create(wide_type, wide_values, 4100));
    const auto exact_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.exact", 1,
        std::span<const FieldDescriptor>(wide_fields).first(4095), {4095, 65536}));
    const auto exact = std::make_shared<const StructValue>(StructValue::create(exact_type, std::span<const NamedValue>(wide_values).first(4095), 4095));
    for (auto& field : wide_fields) field.optional = true;
    const auto sparse_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.sparse", 1,
        std::span<const FieldDescriptor>(wide_fields).first(4095), {4095, 65536}));
    const auto sparse = std::make_shared<const StructValue>(StructValue::create(sparse_type, {}, 0));
    const auto excess_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.sparse_excess", 1, wide_fields, {4100, 65536}));
    const auto excess = std::make_shared<const StructValue>(StructValue::create(excess_type, {}, 0));

    const FieldDescriptor leaf_fields[] = {{"flag", ValueKind::boolean, 0}};
    const auto leaf_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.leaf", 1, leaf_fields, {1, 128}));
    const NamedValue leaf_values[] = {{"flag", Value::from_bool(true)}};
    auto deep = std::make_shared<const StructValue>(StructValue::create(leaf_type, leaf_values, 1));
    std::shared_ptr<const StructValue> exact_depth;
    for (unsigned i = 0; i < 33; ++i) {
        const FieldDescriptor fields[] = {{"child", ValueKind::structure, 0, deep->descriptor()}};
        const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.level" + std::to_string(i), 1, fields, {64, 2048, 64}));
        const NamedValue values[] = {{"child", Value::from_structure(deep)}};
        deep = std::make_shared<const StructValue>(StructValue::create(type, values, 1));
        if (i == 30) exact_depth = deep;
    }
    struct Case { std::shared_ptr<const StructValue> payload; bool accepted; uint64_t fields; };
    std::vector<Case> cases = {{exact, true, 4095}, {wide, false, 0}, {sparse, true, 0}, {excess, false, 0},
        {exact_depth, true, 32}, {deep, false, 0}};

    const auto array_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::boolean, 4094));
    const FieldDescriptor array_fields[] = {FieldDescriptor::array_field("items", array_type)};
    const auto container_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.array_budget", 1, array_fields, {2, 128, 2}));
    const std::vector<Value> elements(4094, Value::from_bool(true));
    for (std::size_t count : {4093, 4094}) {
        const auto array = std::make_shared<const ArrayValue>(ArrayValue::create(array_type, std::span<const Value>(elements).first(count), count));
        const NamedValue values[] = {{"items", Value::from_array(array)}};
        cases.push_back({std::make_shared<const StructValue>(StructValue::create(container_type, values, count)), count == 4093, 1});
    }
    for (const auto& example : cases) {
        InstanceLimits limits; limits.references = 256;
        PluginInstance instance(manifest, data, channel, limits);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK, "bounded observer setup failed");
        channel->publish(example.payload); drain(instance, executor);
        const auto observed = statistics(instance);
        check(observed.events == static_cast<uint64_t>(example.accepted) && observed.failures == static_cast<uint64_t>(!example.accepted) &&
            channel->size() == static_cast<std::size_t>(example.accepted) && !store->references(), "observer rejected exact budget or retained excess delivery");
        if (example.accepted) check(observed.fields == example.fields, "budget boundary lost present field count");
        check(instance.shutdown() == ShutdownResult::closed && !GetModuleHandleW(manifest.entry.c_str()), "bounded observer remains loaded");
        check(!channel->size() && executor.run_one() == RunResult::idle, "bounded observer left a receiver or callback");
    }
}
// 构造明确数量的枚举成员，以最后一项承载待验证的完整整数。
// 步骤：生成独立成员名和数字 → 创建枚举定义及字段 → 选择整数并构造事件。
std::shared_ptr<const StructValue> enum_event(std::size_t count, EnumNumber number) {
    const auto kind = std::holds_alternative<std::int64_t>(number) ? ValueKind::int64 : ValueKind::uint64;
    std::vector<EnumMember> members;
    for (std::size_t i = 0; i < count; ++i) members.push_back({"m" + std::to_string(i), kind == ValueKind::int64 ?
        EnumNumber{static_cast<std::int64_t>(i)} : EnumNumber{static_cast<std::uint64_t>(i)}});
    members.back().number = number;

    const auto definition = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("tests.enum",1,kind,members,count,65536));
    const auto field = FieldDescriptor::enum_field("value",definition);
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.enum_event",1,{&field,1},{count+1,131072}));
    const NamedValue value{"value",Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(definition,number)))};
    return std::make_shared<const StructValue>(StructValue::create(type,{&value,1},8));
}
// 验证实际观察包读取完整枚举整数，预算不足只关闭自身且能够正常卸载。
// 步骤：逐项验收整数极值和成员边界 → 验证两个实例的失败隔离 → 归还所有事件及模块引用。
void enumerations(const Manifest& manifest, const std::filesystem::path& data) {
    auto store = std::make_shared<dfy::events::EventStore>(8,8);
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,2,2);
    FifoExecutor executor(4);
    const std::pair<std::size_t,EnumNumber> cases[] = {{3,INT64_MIN},{3,INT64_MAX},{3,UINT64_MAX},{4094,UINT64_MAX},{4095,UINT64_MAX}};
    for (const auto& [count,number] : cases) {
        PluginInstance instance(manifest,data,channel);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK,"enum observer startup failed");
        channel->publish(enum_event(count,number)); drain(instance,executor);
        const auto observed = statistics(instance); const bool accepted = count <= 4094;
        check(observed.events == static_cast<uint64_t>(accepted) && observed.failures == static_cast<uint64_t>(!accepted) &&
            channel->size() == static_cast<std::size_t>(accepted) && !store->references(),"enum observer budget or cleanup wrong");
        if (accepted) {
            check(observed.fields == 1 && (observed.kinds & (1u << DFY_V2_VALUE_ENUM)),"observer lost enum classification");
            if (const auto* signed_number = std::get_if<std::int64_t>(&number)) check(observed.last_int64 == *signed_number,"observer narrowed signed enum");
            else check(observed.last_uint64 == UINT64_MAX,"observer narrowed unsigned enum");
        }
        check(instance.shutdown() == ShutdownResult::closed && !GetModuleHandleW(manifest.entry.c_str()),"enum observer remains loaded");
    }

    InstanceLimits limits; limits.references = 6;
    PluginInstance failing(manifest,data,channel,limits), healthy(manifest,data,channel);
    check(failing.create() == DFY_V2_OK && failing.start() == DFY_V2_OK && healthy.create() == DFY_V2_OK && healthy.start() == DFY_V2_OK,"enum isolation setup failed");
    channel->publish(enum_event(3,UINT64_MAX)); drain(failing,executor);
    check(channel->size() == 1 && store->references() == 1,"enum failure affected healthy delivery");
    drain(healthy,executor);
    const auto observed = statistics(healthy);
    check(observed.events == 1 && observed.failures == 1 && observed.last_uint64 == UINT64_MAX && !store->references(),"enum failure isolation lost");
    check(failing.shutdown() == ShutdownResult::closed && healthy.shutdown() == ShutdownResult::closed &&
        !GetModuleHandleW(manifest.entry.c_str()) && executor.run_one() == RunResult::idle,"enum isolation retained module or callback");
}
// 将给定内容包入指定分支数的联合，最后分支使用最大标签。
// 步骤：创建独立分支名称 → 校验联合描述及选中内容 → 构造单字段事件。
std::shared_ptr<const StructValue> union_event(std::size_t count, FieldDescriptor content_type, const Value& value) {
    std::vector<UnionBranch> branches;
    for (std::size_t i = 0; i < count; ++i) {
        content_type.name = "b" + std::to_string(i);
        branches.push_back({i,content_type});
    }
    branches.back().tag = UINT64_MAX;
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("tests.union",1,branches,{65536,4194304,64}));
    const auto field = FieldDescriptor::union_field("choice",choice);
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.union_event",1,{&field,1},{65536,4194304,64}));
    const NamedValue input{"choice",Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,UINT64_MAX,value,128)))};
    return std::make_shared<const StructValue>(StructValue::create(type,{&input,1},128));
}
// 验证联合复杂内容、精确访问与深度边界以及独立实例的失败回收。
// 步骤：构造各类分支与边界事件 → 由实际观察包逐项消费 → 核对失败隔离 → 确认回调与模块释放。
void unions(const Manifest& manifest, const std::filesystem::path& data) {
    // 允许深度边界案例的标签正文，避免正文限额遮住待验收的深度拒绝。
    auto store = std::make_shared<dfy::events::EventStore>(8,264);
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,2,2);
    FifoExecutor executor(4);
    const FieldDescriptor integer{"value",ValueKind::uint64,0};
    const auto number = Value::from_uint64(UINT64_MAX);
    const auto composite = composite_value();
    const auto array_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::uint64,1));
    const auto array = std::make_shared<const ArrayValue>(ArrayValue::create(array_type,{&number,1},8));
    const auto map_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::uint64,1,1));
    const MapEntry entry{"k",number}; const auto map = std::make_shared<const MapValue>(MapValue::create(map_type,{&entry,1},9));
    const EnumMember member{"min",INT64_MIN};
    const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("tests.enum",1,ValueKind::int64,{&member,1},1,32));
    struct Example { std::shared_ptr<const StructValue> payload; bool accepted; };
    std::vector<Example> cases{{union_event(4093,integer,number),true},{union_event(4094,integer,number),false},
        {union_event(1,{"text",ValueKind::string,0,{},false,true},Value::null()),true},
        {union_event(1,{"record",ValueKind::structure,0,composite->descriptor()},Value::from_structure(composite)),true},
        {union_event(1,FieldDescriptor::array_field("list",array_type),Value::from_array(array)),true},
        {union_event(1,FieldDescriptor::map_field("lookup",map_type),Value::from_map(map)),true},
        {union_event(1,FieldDescriptor::enum_field("state",enumeration),Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(enumeration,INT64_MIN)))),true}};
    auto deep_value = std::make_shared<const Value>(number); FieldDescriptor deep_type = integer;
    for (unsigned depth = 1; depth <= 31; ++depth) {
        const UnionBranch branch{depth,deep_type};
        const auto type = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("tests.depth"+std::to_string(depth),1,{&branch,1},{64,4096,64}));
        deep_value = std::make_shared<const Value>(Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(type,depth,*deep_value,264))));
        deep_type = FieldDescriptor::union_field("next",type);
        if (depth >= 30) {
            const auto field = FieldDescriptor::union_field("choice",type);
            const auto root = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.deep_union",1,{&field,1},{64,4096,64}));
            const NamedValue input{"choice",*deep_value};
            cases.push_back({std::make_shared<const StructValue>(StructValue::create(root,{&input,1},264)),true});
        }
    }
    const UnionBranch beyond{32,deep_type};
    const auto beyond_type = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("tests.depth32",1,{&beyond,1},{64,4096,64}));
    const auto beyond_value = std::make_shared<const UnionValue>(UnionValue::create(beyond_type,32,*deep_value,264));
    const auto beyond_field = FieldDescriptor::union_field("choice",beyond_type);
    const auto beyond_root = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.excess_union",1,{&beyond_field,1},{64,4096,64}));
    const NamedValue beyond_input{"choice",Value::from_union(beyond_value)};
    cases.push_back({std::make_shared<const StructValue>(StructValue::create(beyond_root,{&beyond_input,1},264)),false});
    for (const auto& example : cases) {
        InstanceLimits limits; limits.references = 256;
        PluginInstance instance(manifest,data,channel,limits);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK,"union observer startup failed");
        channel->publish(example.payload); drain(instance,executor); const auto observed = statistics(instance);
        check(observed.events == static_cast<uint64_t>(example.accepted) && observed.failures == static_cast<uint64_t>(!example.accepted) &&
            channel->size() == static_cast<std::size_t>(example.accepted) && !store->references(),"union observer boundary or cleanup failed");
        if (example.accepted) check((observed.kinds & (1u << DFY_V2_VALUE_UNION)) && observed.fields,"observer lost union kind");
        check(instance.shutdown() == ShutdownResult::closed && !GetModuleHandleW(manifest.entry.c_str()),"union observer remains loaded");
    }

    InstanceLimits limited; limited.references = 6;
    PluginInstance failing(manifest,data,channel,limited), healthy(manifest,data,channel);
    check(failing.create() == DFY_V2_OK && failing.start() == DFY_V2_OK && healthy.create() == DFY_V2_OK && healthy.start() == DFY_V2_OK,"union isolation setup failed");
    const auto payload = union_event(1,integer,number);
    channel->publish(payload); drain(failing,executor);
    check(channel->size() == 1 && store->references() == 1,"union failure blocked healthy receiver");
    drain(healthy,executor);
    check(statistics(healthy).events == 1 && statistics(healthy).failures == 1 && !store->references(),"union failure isolation lost");
    check(failing.shutdown() == ShutdownResult::closed,"failed union observer retained module");
    channel->publish(payload); drain(healthy,executor);
    check(statistics(healthy).events == 2 && healthy.shutdown() == ShutdownResult::closed && !channel->size() &&
        !GetModuleHandleW(manifest.entry.c_str()) && executor.run_one() == RunResult::idle,"union recovery or shutdown failed");
}
// 将已有数组包装为根事件，保持数组完整声明及数据预算。
// 步骤：取得数组描述 → 构造单字段根描述 → 保存事件载荷。
std::shared_ptr<const StructValue> array_event(std::shared_ptr<const ArrayValue> array) {
    const auto field = FieldDescriptor::array_field("items",array->descriptor());
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.array_event",1,{&field,1},{65536,4194304,64}));
    const NamedValue input{"items",Value::from_array(std::move(array))};
    return std::make_shared<const StructValue>(StructValue::create(type,{&input,1},128));
}
// 验证实际包递归消费各类数组元素，空组合值也不能绕过共享访问预算。
// 步骤：构造完整元素矩阵 → 准备精确访问和深度边界 → 逐项消费与卸载 → 检查多实例失败隔离。
void composite_arrays(const Manifest& manifest, const std::filesystem::path& data) {
    auto store = std::make_shared<dfy::events::EventStore>(8,128);
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,2,2);
    FifoExecutor executor(4);
    const auto number = Value::from_uint64(UINT64_MAX); const auto record = composite_value();
    const EnumMember member{"min",INT64_MIN};
    const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("tests.enum",1,ValueKind::int64,{&member,1},1,32));
    const UnionBranch branch{UINT64_MAX,{"number",ValueKind::uint64,0}};
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("tests.choice",1,{&branch,1},{1,32}));
    const auto inner = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::uint64,1));
    const auto map = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::uint64,1,1));
    const MapEntry entry{"k",number};
    struct Input { FieldDescriptor schema; Value value; };
    const Input examples[] = {{{"",ValueKind::structure,0,record->descriptor()},Value::from_structure(record)},
        {FieldDescriptor::enum_field("",enumeration),Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(enumeration,INT64_MIN)))},
        {FieldDescriptor::union_field("",choice),Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,UINT64_MAX,number,16)))},
        {FieldDescriptor::array_field("",inner),Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(inner,{&number,1},8)))},
        {FieldDescriptor::map_field("",map),Value::from_map(std::make_shared<const MapValue>(MapValue::create(map,{&entry,1},9)))}};
    struct Example { std::shared_ptr<const StructValue> payload; bool accepted; };
    std::vector<Example> cases;
    for (const auto& example : examples) {
        auto schema = example.schema; schema.nullable = true;
        const auto type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ElementDescriptor::create(schema,{64,1024,64}),2));
        const Value values[] = {example.value,Value::null()};
        cases.push_back({array_event(std::make_shared<const ArrayValue>(ArrayValue::create(type,values,128))),true});
        cases.push_back({array_event(std::make_shared<const ArrayValue>(ArrayValue::create(type,{},0))),true});
    }

    const auto empty_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::boolean,0));
    const auto empty = Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(empty_type,{},0)));
    const auto repeated_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::array_field("",empty_type,false,true),{2,0,2}),2047));
    for (bool exact : {true,false}) {
        std::vector<Value> elements; elements.push_back(exact ? Value::null() : empty);
        for (unsigned i = 1; i < 2047; ++i) elements.push_back(empty);
        cases.push_back({array_event(std::make_shared<const ArrayValue>(ArrayValue::create(repeated_type,elements,0))),exact});
    }
    auto deep_type = inner;
    auto deep = std::make_shared<const ArrayValue>(ArrayValue::create(deep_type,{&number,1},8));
    for (unsigned depth = 2; depth <= 32; ++depth) {
        deep_type = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(
            ElementDescriptor::create(FieldDescriptor::array_field("",deep_type),{64,0,64}),1));
        const Value value = Value::from_array(deep);
        deep = std::make_shared<const ArrayValue>(ArrayValue::create(deep_type,{&value,1},8));
        if (depth >= 31) cases.push_back({array_event(deep),depth == 31});
    }
    for (const auto& example : cases) {
        InstanceLimits limits; limits.references = 256;
        PluginInstance instance(manifest,data,channel,limits);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK,"composite array observer setup failed");
        channel->publish(example.payload); drain(instance,executor); const auto observed = statistics(instance);
        check(observed.events == static_cast<uint64_t>(example.accepted) && observed.failures == static_cast<uint64_t>(!example.accepted) &&
            channel->size() == static_cast<std::size_t>(example.accepted) && !store->references(),"composite array observer boundary or cleanup failed");
        if (example.accepted) check(observed.kinds & (1u << DFY_V2_VALUE_ARRAY),"observer lost array classification");
        check(instance.shutdown() == ShutdownResult::closed && !GetModuleHandleW(manifest.entry.c_str()),"composite array observer remains loaded");
    }
    InstanceLimits limited; limited.references = 6;
    PluginInstance failing(manifest,data,channel,limited), healthy(manifest,data,channel);
    check(failing.create() == DFY_V2_OK && failing.start() == DFY_V2_OK && healthy.create() == DFY_V2_OK && healthy.start() == DFY_V2_OK,"array isolation setup failed");
    channel->publish(cases.front().payload); drain(failing,executor);
    check(channel->size() == 1 && store->references() == 1,"array failure blocked healthy observer");
    drain(healthy,executor);
    check(statistics(healthy).events == 1 && statistics(healthy).failures == 1 && !store->references(),"array failure isolation lost");
    check(failing.shutdown() == ShutdownResult::closed && healthy.shutdown() == ShutdownResult::closed &&
        !GetModuleHandleW(manifest.entry.c_str()) && executor.run_one() == RunResult::idle,"array observer leaked after failure");
}
// 将完整映射包装为事件，键正文与实际值一同受载荷预算约束。
// 步骤：读取映射描述 → 构造根字段约束 → 保存不可变载荷。
std::shared_ptr<const StructValue> map_event(std::shared_ptr<const MapValue> map) {
    const auto field = FieldDescriptor::map_field("items",map->descriptor());
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.map_event",1,{&field,1},{65536,4194304,64}));
    const NamedValue input{"items",Value::from_map(std::move(map))};
    return std::make_shared<const StructValue>(StructValue::create(type,{&input,1},65536));
}
// 验证实际观察包的完整映射值、交替容器及精确消费边界。
// 步骤：构造复杂值矩阵 → 准备共享空映射和深度边界 → 消费并卸载 → 验证失败实例隔离。
void composite_maps(const Manifest& manifest, const std::filesystem::path& data) {
    auto store = std::make_shared<dfy::events::EventStore>(8,65536);
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,2,2);
    FifoExecutor executor(4);
    const auto number = Value::from_uint64(UINT64_MAX); const auto record = composite_value();
    const EnumMember member{"min",INT64_MIN};
    const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("tests.enum",1,ValueKind::int64,{&member,1},1,32));
    const UnionBranch branch{UINT64_MAX,{"number",ValueKind::uint64,0}};
    const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("tests.choice",1,{&branch,1},{1,32}));
    const auto inner_map = std::make_shared<const MapDescriptor>(MapDescriptor::create(
        ElementDescriptor::create({"",ValueKind::structure,0,record->descriptor()},{64,1024,64}),1,1));
    const MapEntry entry{"k",Value::from_structure(record)};
    const auto nested_map = Value::from_map(std::make_shared<const MapValue>(MapValue::create(inner_map,{&entry,1},128)));
    const auto array = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::map_field("",inner_map),{64,1024,64}),1));
    struct Input { FieldDescriptor schema; Value value; };
    const Input examples[] = {{{"",ValueKind::structure,0,record->descriptor()},Value::from_structure(record)},
        {FieldDescriptor::enum_field("",enumeration),Value::from_enum(std::make_shared<const EnumValue>(EnumValue::create(enumeration,INT64_MIN)))},
        {FieldDescriptor::union_field("",choice),Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice,UINT64_MAX,number,16)))},
        {FieldDescriptor::map_field("",inner_map),nested_map},
        {FieldDescriptor::array_field("",array),Value::from_array(std::make_shared<const ArrayValue>(ArrayValue::create(array,{&nested_map,1},128)))}};
    struct Example { std::shared_ptr<const StructValue> payload; bool accepted; };
    std::vector<Example> cases;
    for (const auto& example : examples) {
        auto schema = example.schema; schema.nullable = true;
        const auto type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ElementDescriptor::create(schema,{64,1024,64}),2,3));
        const MapEntry values[] = {{std::string("a\0b",3),example.value},{"nil",Value::null()}};
        cases.push_back({map_event(std::make_shared<const MapValue>(MapValue::create(type,values,128))),true});
        cases.push_back({map_event(std::make_shared<const MapValue>(MapValue::create(type,{},0))),true});
    }

    const auto empty_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::boolean,0,0));
    const auto empty = Value::from_map(std::make_shared<const MapValue>(MapValue::create(empty_type,{},0)));
    const auto repeated_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::map_field("",empty_type,false,true),{2,0,2}),2047,4));
    for (bool exact : {true,false}) {
        std::vector<MapEntry> entries; entries.push_back({"0",exact ? Value::null() : empty});
        for (unsigned i = 1; i < 2047; ++i) entries.push_back({std::to_string(i),empty});
        cases.push_back({map_event(std::make_shared<const MapValue>(MapValue::create(repeated_type,entries,65536))),exact});
    }
    auto deep_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::uint64,1,0));
    const MapEntry leaf{"",number}; auto deep = std::make_shared<const MapValue>(MapValue::create(deep_type,{&leaf,1},8));
    for (unsigned depth = 2; depth <= 32; ++depth) {
        deep_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(
            ElementDescriptor::create(FieldDescriptor::map_field("",deep_type),{64,0,64}),1,0));
        const MapEntry value{"",Value::from_map(deep)};
        deep = std::make_shared<const MapValue>(MapValue::create(deep_type,{&value,1},8));
        if (depth >= 31) cases.push_back({map_event(deep),depth == 31});
    }
    for (const auto& example : cases) {
        InstanceLimits limits; limits.references = 256;
        PluginInstance instance(manifest,data,channel,limits);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK,"composite map observer setup failed");
        channel->publish(example.payload); drain(instance,executor); const auto observed = statistics(instance);
        check(observed.events == static_cast<uint64_t>(example.accepted) && observed.failures == static_cast<uint64_t>(!example.accepted) &&
            channel->size() == static_cast<std::size_t>(example.accepted) && !store->references(),"composite map observer boundary or cleanup failed");
        if (example.accepted) check(observed.kinds & (1u << DFY_V2_VALUE_MAP),"observer lost map classification");
        check(instance.shutdown() == ShutdownResult::closed && !GetModuleHandleW(manifest.entry.c_str()),"composite map observer remains loaded");
    }
    InstanceLimits limited; limited.references = 6;
    PluginInstance failing(manifest,data,channel,limited), healthy(manifest,data,channel);
    check(failing.create() == DFY_V2_OK && failing.start() == DFY_V2_OK && healthy.create() == DFY_V2_OK && healthy.start() == DFY_V2_OK,"map isolation setup failed");
    channel->publish(cases.front().payload); drain(failing,executor);
    check(channel->size() == 1 && store->references() == 1,"map failure blocked healthy observer");
    drain(healthy,executor);
    check(statistics(healthy).events == 1 && statistics(healthy).failures == 1 && !store->references(),"map failure isolation lost");
    check(failing.shutdown() == ShutdownResult::closed && healthy.shutdown() == ShutdownResult::closed &&
        !GetModuleHandleW(manifest.entry.c_str()) && executor.run_one() == RunResult::idle,"map observer leaked after failure");
}
// 验证实际观察包通过组能力共享领取、退出和处理失败，不关闭其他成员。
// 步骤：验证广播与组共同消费 → 停止单个成员后继续 → 撤销排队回调 → 验证失败隔离及关闭组拒绝。
void consumer_groups(const Manifest& manifest, const std::filesystem::path& data) {
    auto store = std::make_shared<dfy::events::EventStore>(8,128);
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,2,2);
    const auto group = channel->subscribe_group(2); const auto independent = channel->subscribe();
    FifoExecutor executor(4);
    {
        PluginInstance first(manifest,data,channel,{},group), second(manifest,data,channel,{},group);
        check(first.create() == DFY_V2_OK && first.start() == DFY_V2_OK && second.create() == DFY_V2_OK && second.start() == DFY_V2_OK &&
            group->member_count() == 2 && channel->size() == 2,"group observers created independent subscriptions");
        const auto id = channel->publish(event_value());
        check(!statistics(first).events,"group publication entered DLL inline");
        drain(first,executor); drain(second,executor);
        check(statistics(second).events == 1 && statistics(second).last_sequence == id.sequence && independent->poll().event.get().id == id,
            "group observer duplicated delivery or disagreed with broadcast");
        check(first.shutdown() == ShutdownResult::closed && group->member_count() == 1 && group->state() == dfy::events::QueueState::open,
            "first observer stopped shared group");
        const auto next = channel->publish_to_group(event_value(),group); drain(second,executor);
        check(statistics(second).events == 2 && statistics(second).last_sequence == next.sequence && !independent->size() && !store->references(),
            "remaining group observer did not continue");
        check(second.shutdown() == ShutdownResult::closed && !group->member_count() && !GetModuleHandleW(manifest.entry.c_str()),"group observers did not unload");
    }
    independent->close(dfy::events::CloseMode::cancel); channel->remove(independent);
    {
        PluginInstance first(manifest,data,channel,{},group), second(manifest,data,channel,{},group), excess(manifest,data,channel,{},group);
        check(first.create() == DFY_V2_OK && first.start() == DFY_V2_OK && second.create() == DFY_V2_OK && second.start() == DFY_V2_OK &&
            excess.create() == DFY_V2_OK && excess.start() == DFY_V2_RESOURCE_EXHAUSTED && excess.shutdown() == ShutdownResult::closed && group->member_count() == 2,
            "group member admission failure leaked or affected peers");
        const auto skipped = channel->publish_to_group(event_value(),group);
        check(first.advance(executor) == ScheduleResult::queued && !statistics(second).events,"group queued callback setup failed");
        check(first.shutdown() == ShutdownResult::closed && group->member_count() == 1 && executor.run_one() == RunResult::executed,
            "queued group callback prevented safe member stop");
        drain(second,executor);
        check(!statistics(second).events && !statistics(second).failures && !store->references(),"revoked group callback entered DLL or replayed claimed event");
        const auto next = channel->publish_to_group(event_value(),group); drain(second,executor);
        check(next.sequence == skipped.sequence + 1 && statistics(second).events == 1 && statistics(second).last_sequence == next.sequence,
            "group reload path repeated old delivery");
        check(second.shutdown() == ShutdownResult::closed && !group->member_count() && !GetModuleHandleW(manifest.entry.c_str()),"queued group path retained DLL");
    }
    {
        InstanceLimits limited; limited.references = 6;
        PluginInstance failing(manifest,data,channel,limited,group), healthy(manifest,data,channel,{},group);
        check(failing.create() == DFY_V2_OK && failing.start() == DFY_V2_OK && healthy.create() == DFY_V2_OK && healthy.start() == DFY_V2_OK,"group failure setup failed");
        const auto failed = channel->publish_to_group(composite_value(),group); drain(failing,executor); drain(healthy,executor);
        check(group->member_count() == 1 && !statistics(healthy).events && statistics(healthy).failures == 1 && !store->references(),
            "failed group member closed peers or replayed delivery");
        const auto next = channel->publish_to_group(composite_value(),group); drain(healthy,executor);
        check(next.sequence == failed.sequence + 1 && statistics(healthy).events == 1 && statistics(healthy).last_sequence == next.sequence,
            "healthy group member did not consume next event");
        check(failing.shutdown() == ShutdownResult::closed && healthy.shutdown() == ShutdownResult::closed &&
            !group->member_count() && !GetModuleHandleW(manifest.entry.c_str()),"failed group member retained module");
    }
    group->close(dfy::events::CloseMode::cancel);
    {
        PluginInstance closed(manifest,data,channel,{},group);
        check(closed.create() == DFY_V2_OK && closed.start() == DFY_V2_BUSY && closed.shutdown() == ShutdownResult::closed && channel->size() == 1,
            "closed group silently fell back to independent subscription");
    }
    channel->remove_group(group);
    check(!channel->size() && !store->references() && executor.run_one() == RunResult::idle && PluginInstance::retained().empty(),"group lifecycle leaked resources");
}
enum class GroupFault { absent, failed_pointer, success_null, short_table, long_table, wrong_version, no_join, no_leave, no_read, unsupported_table };
GroupFault group_fault = GroupFault::absent;
dfy_v2_group_api group_fault_table{};
// 只替换组能力查询结果，其他能力仍由实际宿主提供，不创建任何伪成员。
// 步骤：分流非组查询 → 清空输出 → 按故障模式返回状态或受控表。
dfy_v2_status DFY_V2_CALL group_fault_query(dfy_v2_string_view name, uint32_t version, const void** output, dfy_v2_error* error) {
    if (!name.data || name.length != sizeof(DFY_V2_GROUP_INTERFACE)-1 || std::string_view(name.data,name.length) != DFY_V2_GROUP_INTERFACE)
        return HostServices::query(name,version,output,error);
    if (error) error->length = 0;
    if (!output) return DFY_V2_INVALID_ARGUMENT;
    *output = nullptr;
    if (group_fault == GroupFault::absent) return DFY_V2_UNSUPPORTED;
    if (group_fault == GroupFault::failed_pointer) { *output = reinterpret_cast<const void*>(uintptr_t{1}); return DFY_V2_FAILED; }
    if (group_fault == GroupFault::success_null) return DFY_V2_OK;
    *output = &group_fault_table;
    return group_fault == GroupFault::unsupported_table ? DFY_V2_UNSUPPORTED : DFY_V2_OK;
}
// 验证观察包只对明确缺省进行独立订阅回退，异常或错误组表必须拒绝创建。
// 步骤：装配真实能力 → 注入状态、布局及缺失入口故障 → 核对无半实例 → 验证缺省路径并卸载。
void group_negotiation(const Manifest& manifest) {
    dfy::platform::CodeModule module(manifest.entry,8); auto code = module.lease();
    const auto api = ability<dfy_v2_lifecycle_api>(code,DFY_V2_LIFECYCLE_INTERFACE);
    auto store = std::make_shared<dfy::events::EventStore>(4,64);
    auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,1,1);
    const auto group = std::make_shared<dfy::events::ConsumerGroup>(store,1,1);
    HostServices host(32); host.enable_events(channel,4); host.enable_group(group,4); host.enable_notifications(code.retain(),4);
    {
        auto binding = host.bind(); const void* table = nullptr;
        check(HostServices::query({DFY_V2_GROUP_INTERFACE,sizeof(DFY_V2_GROUP_INTERFACE)-1},1,&table,nullptr) == DFY_V2_OK,"group fault fixture missing service");
        const auto valid = *static_cast<const dfy_v2_group_api*>(table);
        for (const auto fault : {GroupFault::failed_pointer,GroupFault::success_null,GroupFault::short_table,GroupFault::long_table,
            GroupFault::wrong_version,GroupFault::no_join,GroupFault::no_leave,GroupFault::no_read,GroupFault::unsupported_table}) {
            group_fault = fault; group_fault_table = valid;
            if (fault == GroupFault::short_table) group_fault_table.header.struct_size = 8;
            if (fault == GroupFault::long_table) group_fault_table.header.struct_size += 8;
            if (fault == GroupFault::wrong_version) group_fault_table.header.interface_version = 2;
            if (fault == GroupFault::no_join) group_fault_table.join = nullptr;
            if (fault == GroupFault::no_leave) group_fault_table.leave = nullptr;
            if (fault == GroupFault::no_read) group_fault_table.read = nullptr;
            dfy_v2_instance* instance = reinterpret_cast<dfy_v2_instance*>(uintptr_t{1});
            auto call = code.enter();
            const auto status = api.create(group_fault_query,{"{}",2},&instance,nullptr);
            check(status == (fault == GroupFault::failed_pointer ? DFY_V2_FAILED : DFY_V2_UNSUPPORTED) && !instance &&
                !host.references() && !host.resources() && !group->member_count() && !channel->size(),"invalid group query silently fell back or left partial instance");
        }
        check(!statistics(code).created,"failed group negotiation counted an instance");
        group_fault = GroupFault::absent; dfy_v2_instance* instance = nullptr;
        {
            auto call = code.enter();
            check(api.create(group_fault_query,{"{}",2},&instance,nullptr) == DFY_V2_OK && instance && api.start(instance,nullptr) == DFY_V2_OK &&
                channel->size() == 1 && !group->member_count(),"explicit absent group did not use independent subscription");
            uint32_t state = 0;
            check(api.request_stop(instance,nullptr) == DFY_V2_OK && api.poll_stop(instance,&state,nullptr) == DFY_V2_OK && state == DFY_V2_STOPPED,
                "group negotiation control instance did not stop");
            api.destroy(instance);
        }
    }
    check(host.begin_shutdown() && host.finish_shutdown() && !channel->size() && !store->references(),"group negotiation retained host work");
    code.reset();
    check(module.close() == dfy::platform::ModuleClose::closed && !GetModuleHandleW(manifest.entry.c_str()),"group negotiation retained DLL");
}
// 验证排队时卸载后宿主包装不进入已释放的 DLL，并实际重新加载新实例。
// 步骤：创建并排队通知 → 卸载后执行剩余包装 → 重新加载 → 消费新事件并再次卸载。
void reload(const Manifest& manifest, const std::filesystem::path& data, const std::shared_ptr<dfy::events::BroadcastChannel>& channel,
    const std::shared_ptr<dfy::events::EventStore>& store) {
    FifoExecutor executor(2);
    for (unsigned round = 0; round < 2; ++round) {
        PluginInstance instance(manifest, data, channel);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK && statistics(instance).created == 1, "fresh reload failed");
        const auto id = channel->publish(event_value());
        if (!round) {
            check(instance.advance(executor) == ScheduleResult::queued && !statistics(instance).events, "queue setup failed");
        } else {
            drain(instance, executor);
            check(statistics(instance).events == 1 && statistics(instance).last_sequence == id.sequence, "reload consumed stale event");
        }
        check(instance.shutdown() == ShutdownResult::closed && !GetModuleHandleW(manifest.entry.c_str()), "reload shutdown retained module");
        if (!round) check(executor.run_one() == RunResult::executed, "revoked host wrapper did not finish");
        check(!channel->size() && !store->references() && executor.run_one() == RunResult::idle, "reload left work or data");
    }
}
// 在真实插件停止入口主动取消尚未消费的等待，区分取消与宿主预先关闭。
// 步骤：创建启动 → 发布但不推进 → 插件请求取消与停止 → 核对取消结果 → 销毁并释放。
void cancellation(const Manifest& manifest, const std::shared_ptr<dfy::events::BroadcastChannel>& channel,
    const std::shared_ptr<dfy::events::EventStore>& store) {
    dfy::platform::CodeModule module(manifest.entry, 8);
    auto code = module.lease();
    const auto api = ability<dfy_v2_lifecycle_api>(code, DFY_V2_LIFECYCLE_INTERFACE);
    check(api.create && api.start && api.request_stop && api.poll_stop && api.destroy, "incomplete observer lifecycle");
    HostServices host(32);
    host.enable_events(channel, 4); host.enable_notifications(code.retain(), 8);
    dfy_v2_instance* instance = nullptr;
    {
        auto binding = host.bind(); auto call = code.enter();
        check(api.create(HostServices::query, {"{}",2}, &instance, nullptr) == DFY_V2_OK && instance &&
            api.start(instance, nullptr) == DFY_V2_OK, "manual observer start failed");
    }
    channel->publish(event_value());
    {
        auto binding = host.bind(); auto call = code.enter();
        uint32_t state = 0;
        check(api.request_stop(instance, nullptr) == DFY_V2_OK && api.request_stop(instance, nullptr) == DFY_V2_OK &&
            api.poll_stop(instance, &state, nullptr) == DFY_V2_OK && state == DFY_V2_STOPPED, "observer cancellation failed");
    }
    check(host.begin_shutdown() && !host.resources() && !store->references() && !channel->size(), "cancelled observer retained work");
    {
        auto binding = host.bind(); auto call = code.enter(); api.destroy(instance);
    }
    check(host.finish_shutdown(), "manual observer finalization failed");
    const auto observed = statistics(code);
    check(observed.created == 1 && observed.destroyed == 1 && observed.cancelled == 1 && !observed.events && !observed.failures,
        "cancelled waiting was confused with consumption or replayed");
    code.reset();
    check(module.close() == dfy::platform::ModuleClose::closed && !GetModuleHandleW(manifest.entry.c_str()), "manual observer DLL remains");
}
// 验证消费预算失败只关闭自身，部分启动和错误配置均能独立回收。
// 步骤：运行受限与正常实例 → 观察失败隔离 → 验证启动预算失败 → 验证非法配置。
void failures(const Manifest& manifest, const std::filesystem::path& data, const std::shared_ptr<dfy::events::BroadcastChannel>& channel,
    const std::shared_ptr<dfy::events::EventStore>& store) {
    FifoExecutor executor(4);
    InstanceLimits limited; limited.references = 5;
    {
        PluginInstance failing(manifest, data, channel, limited), healthy(manifest, data, channel);
        check(failing.create() == DFY_V2_OK && failing.start() == DFY_V2_OK && healthy.create() == DFY_V2_OK && healthy.start() == DFY_V2_OK,
            "failure isolation setup failed");
        channel->publish(event_value());
        drain(failing, executor);
        check(channel->size() == 1 && store->references() == 1, "failed observer blocked or retained other delivery");
        drain(healthy, executor);
        const auto observed = statistics(healthy);
        check(observed.events == 1 && observed.failures == 1 && !store->references(), "observer failure changed healthy result");
        check(failing.shutdown() == ShutdownResult::closed, "failed observer did not stop");
        channel->publish(event_value()); drain(healthy, executor);
        check(statistics(healthy).events == 2 && healthy.shutdown() == ShutdownResult::closed, "healthy observer did not continue");
    }
    limited.references = 2;
    {
        PluginInstance partial(manifest, data, channel, limited);
        check(partial.create() == DFY_V2_OK && partial.start() == DFY_V2_RESOURCE_EXHAUSTED && !channel->size() &&
            partial.shutdown() == ShutdownResult::closed, "partial observer start leaked");
    }
    const auto invalid = data / "invalid";
    std::filesystem::create_directories(invalid / "plugins" / manifest.id);
    std::ofstream settings(invalid / "plugins" / manifest.id / "settings.json"); settings << R"({"unsupported":true})"; settings.close();
    check(static_cast<bool>(settings), "invalid settings fixture not written");
    {
        PluginInstance rejected(manifest, invalid, channel);
        check(rejected.create() == DFY_V2_INVALID_ARGUMENT && rejected.phase() == InstancePhase::unloaded, "invalid observer configuration accepted");
    }
    channel->publish(event_value());
    check(!channel->size() && !store->references() && !GetModuleHandleW(manifest.entry.c_str()) && PluginInstance::retained().empty(),
        "failure paths leaked or empty-plugin publication broke");
}
}
// 验收实际观察包的完整类型、组消费、能力边界和安全退出。
// 步骤：装配并验收基础消费 → 验证重载、取消和隔离 → 逐类验收组合预算 → 验收组消费及能力协商 → 确认回收结果。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3, "expected observer package and userdata path");
        const auto manifest = load_manifest(argv[1]);
        auto store = std::make_shared<dfy::events::EventStore>(16, 64);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store, 4, 4);
        FifoExecutor executor(4);
        PluginInstance instance(manifest, argv[2], channel);
        check(instance.create() == DFY_V2_OK && instance.start() == DFY_V2_OK, "observer startup failed");
        const auto id = channel->publish(event_value());
        check(statistics(instance).events == 0, "publish invoked observer inline");
        drain(instance, executor);
        const auto observed = statistics(instance);
        check(observed.created == 1 && observed.events == 1 && observed.fields == 6 && observed.bytes == 11 && observed.kinds == 126 &&
            observed.last_bool == 1 && observed.last_int64 == INT64_MIN && observed.last_uint64 == UINT64_MAX &&
            std::signbit(observed.last_float64) && observed.last_domain == id.domain && observed.last_sequence == id.sequence && !observed.failures,
            "observer reflection lost data or precision");
        check(!store->references(), "observer retained event data");
        check(instance.shutdown() == ShutdownResult::closed && !channel->size() && !GetModuleHandleW(manifest.entry.c_str()),
            "observer did not unload");
        check(PluginInstance::retained().empty(), "observer retained failed instance");
        reload(manifest, argv[2], channel, store);
        cancellation(manifest, channel, store);
        failures(manifest, argv[2], channel, store);
        composites(manifest, argv[2]);
        enumerations(manifest, argv[2]);
        unions(manifest, argv[2]);
        composite_arrays(manifest, argv[2]);
        composite_maps(manifest, argv[2]);
        consumer_groups(manifest, argv[2]);
        group_negotiation(manifest);
        std::cout << "Independent observer checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
