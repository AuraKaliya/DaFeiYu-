#include <dafeiyu/host_services.hpp>
#include <dafeiyu/lifecycle_abi_v2.h>
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_lifecycle_api) == 48 && offsetof(dfy_v2_lifecycle_api, destroy) == 40);
namespace {
// 在生命周期请求、完成或回收不符合约定时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}
// 用真实 C 实例验证生命周期接口，宿主服务在所有入口期间有效。
// 步骤：查询能力 → 创建并启动真实资源 → 注入停止失败及延后结果 → 确认停止后销毁。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected lifecycle DLL");
        dfy::platform::CodeModule module(std::filesystem::canonical(argv[1]), 4);
        auto code = module.lease();
        const dfy_v2_lifecycle_api* api = nullptr;
        void (DFY_V2_CALL *mode)(uint32_t) = nullptr;
        uint32_t (DFY_V2_CALL *count)(uint32_t) = nullptr;
        {
            auto call = code.enter();
            const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
            const dfy_v2_plugin_api* bootstrap = nullptr;
            check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap, "bootstrap failed");
            const void* table = nullptr;
            check(bootstrap->query_interface({DFY_V2_LIFECYCLE_INTERFACE, sizeof(DFY_V2_LIFECYCLE_INTERFACE)-1}, 1, &table, nullptr) == DFY_V2_OK,
                "lifecycle missing");
            api = static_cast<const dfy_v2_lifecycle_api*>(table);
            check(api && api->header.struct_size == sizeof(*api) && api->header.interface_version == 1 &&
                api->create && api->start && api->request_stop && api->poll_stop && api->destroy, "invalid lifecycle table");
            mode = reinterpret_cast<decltype(mode)>(call.symbol("dfy_lifecycle_mode"));
            count = reinterpret_cast<decltype(count)>(call.symbol("dfy_lifecycle_count"));
        }
        auto store = std::make_shared<dfy::events::EventStore>(4, 0);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store, 1, 1);
        dfy::plugins::HostServices host(8);
        host.enable_events(channel, 2);
        dfy_v2_instance* instance = nullptr;
        {
            auto binding = host.bind(); auto call = code.enter();
            mode(1);
            check(api->create(dfy::plugins::HostServices::query, {"{}",2}, &instance, nullptr) == DFY_V2_FAILED && !instance,
                "failed creation retained instance");
            check(api->create(dfy::plugins::HostServices::query, {"{}",2}, &instance, nullptr) == DFY_V2_OK && instance, "create failed");
            mode(2);
            check(api->start(instance, nullptr) == DFY_V2_FAILED && channel->size() == 1, "partial start not exercised");
        }
        check(host.begin_shutdown() && !channel->size(), "host did not quiesce partial start");
        {
            auto binding = host.bind(); auto call = code.enter();
            mode(3);
            check(api->request_stop(instance, nullptr) == DFY_V2_FAILED && count(3) == 0, "stop failure destroyed instance");
            mode(4);
            check(api->request_stop(instance, nullptr) == DFY_V2_OK && api->request_stop(instance, nullptr) == DFY_V2_OK, "stop retry failed");
            uint32_t state = 99;
            check(api->poll_stop(instance, &state, nullptr) == DFY_V2_OK && state == DFY_V2_STOP_PENDING && !count(3), "accepted stop reported done");
            check(api->poll_stop(instance, &state, nullptr) == DFY_V2_OK && state == DFY_V2_STOPPED, "stop did not finish");
            api->destroy(instance); instance = nullptr;
            check(count(0) == 1 && count(1) == 1 && count(2) == 2 && count(3) == 1, "lifecycle replayed a completed stage");
        }
        check(host.finish_shutdown(), "host cleanup incomplete");
        code.reset();
        check(module.close() == dfy::platform::ModuleClose::closed, "module reference remains");
        std::cout << "ABI v2 lifecycle checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
