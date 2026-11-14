#include <dafeiyu/plugin_instance.hpp>
#include <windows.h>
#pragma pack(push, 1)
#include "../plugins/examples/observer/observer_api.h"
struct PackingProbe { char tag; uint64_t value; };
#pragma pack(pop)
#include <cmath>
#include <iostream>
#include <dafeiyu/enum_value.hpp>
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
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.composite", 1, fields, {7, 256, 2}));
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
    const auto container_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.array_budget", 1, array_fields, {1, 128, 2}));
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
// 使用实际目录包和纯 C 观察 DLL 验收创建、反射消费与安全卸载。
// 步骤：读取包并装配 → 创建启动 → 发布并显式推进 → 核对精度与引用 → 停止销毁并确认卸载。
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
        std::cout << "Independent observer checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
