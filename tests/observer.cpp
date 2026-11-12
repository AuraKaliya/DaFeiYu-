#include <dafeiyu/plugin_instance.hpp>
#include <windows.h>
#pragma pack(push, 1)
#include "../plugins/examples/observer/observer_api.h"
struct PackingProbe { char tag; uint64_t value; };
#pragma pack(pop)
#include <cmath>
#include <iostream>
static_assert(sizeof(PackingProbe) == 9 && sizeof(dfy_observer_stats) == 112 && alignof(dfy_observer_stats) == 8);
namespace {
using namespace dfy::plugins;
using namespace dfy::runtime;
// 在独立观察插件的行为或回收不符合预期时报告失败。
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
// 在代码与活动保护内取得只读统计，不保留 DLL 表地址。
// 步骤：建立保护 → 查询引导与统计能力 → 复制统计。
dfy_observer_stats statistics(PluginInstance& instance) {
    auto code = instance.retain_code(); auto call = code.enter();
    const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
    const dfy_v2_plugin_api* bootstrap = nullptr;
    check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap && bootstrap->query_interface, "observer query failed");
    const void* table = nullptr;
    check(bootstrap->query_interface({DFY_OBSERVER_STATS_INTERFACE, sizeof(DFY_OBSERVER_STATS_INTERFACE)-1}, 1, &table, nullptr) == DFY_V2_OK,
        "observer statistics missing");
    const auto* api = static_cast<const dfy_observer_stats_api*>(table);
    check(api && api->header.struct_size == sizeof(*api) && api->header.interface_version == 1 && api->read, "invalid statistics table");
    dfy_observer_stats result{};
    check(api->read(&result, nullptr) == DFY_V2_OK, "statistics read failed");
    return result;
}
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
        std::cout << "Independent observer checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
