#include <dafeiyu/host_services.hpp>
#include "fixtures/query_v2/api.h"
#include <array>
#include <iostream>

namespace {
using namespace dfy::plugins;
using namespace dfy::runtime;
// 在代码寿命、通知或资源责任不符合约定时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct Probe { dfy::platform::CodeModule* module; dfy::platform::ModuleClose result; std::size_t active = 0; HostServices* host = nullptr; bool shutdown = false; };
// 在真实 C 原生栈内请求关闭，记录活动计量，不抛出跨 ABI 异常。
void DFY_V2_CALL probe(void* user) {
    auto& value = *static_cast<Probe*>(user);
    value.active = value.module->active_calls();
    value.result = value.module->close();
    if (value.host) value.shutdown = value.host->begin_shutdown();
}
// 查询当前绑定的宿主能力，测试不保存任何实例裸指针。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(), name.size()}, 1, &table, nullptr) == DFY_V2_OK && table, "service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1, "service mismatch");
    return *static_cast<const T*>(table);
}
}

// 以真实 C 回调验收生产通知、宿主绑定、活动保护和资源收尾。
// 步骤：取得受控回调 → 验证容量回滚 → 延后交付与新登记 → 撤销排队项 → 释放客户引用后继续交付并回收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected C callback DLL");
        dfy::platform::CodeModule module(std::filesystem::canonical(argv[1]), 12);
        dfy_v2_completion_fn callback = nullptr;
        {
            auto code = module.lease(); auto call = code.enter();
            const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
            const dfy_v2_plugin_api* bootstrap = nullptr;
            check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap, "query failed");
            const void* table = nullptr;
            check(bootstrap->query_interface({DFY_TEST_NOTIFY_ID, sizeof(DFY_TEST_NOTIFY_ID) - 1}, 1, &table, nullptr) == DFY_V2_OK,
                "callback fixture missing");
            const auto* api = static_cast<const dfy_test_notify_api*>(table);
            check(api && api->header.struct_size == sizeof(*api) && api->header.interface_version == 1 && api->callback, "invalid callback fixture");
            callback = api->callback;
        }
        {
            HostServices host(20);
            host.enable_notifications(module.lease(), 4);
            auto store = std::make_shared<dfy::events::EventStore>(2, 0);
            auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store, 1);
            auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue, 2);
            const auto receiver = host.expose(reader);
            const dfy_v2_operation_notify_api* notices = nullptr;
            const dfy_v2_reference_api* refs = nullptr;
            dfy_v2_handle request{};
            dfy_test_notify_context parent{}, child{};
            Probe observed{&module, dfy::platform::ModuleClose::closed};
            observed.host = &host;
            {
                auto binding = host.bind();
                notices = &service<dfy_v2_operation_notify_api>(DFY_V2_OPERATION_NOTIFY_INTERFACE);
                refs = &service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
                check(service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).next(receiver, &request, nullptr) == DFY_V2_OK, "read failed");
                std::array<dfy_v2_handle, 4> held{};
                for (auto& handle : held) check(notices->subscribe(request, callback, nullptr, &handle, nullptr) == DFY_V2_OK, "fill notification budget failed");
                const auto count = host.references();
                dfy_v2_handle rejected{};
                check(notices->subscribe(request, callback, nullptr, &rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id &&
                    host.resources() == 5 && host.references() == count, "notification failure left partial resources");
                for (const auto handle : held) { notices->revoke(handle, nullptr); refs->release(handle, nullptr); }
                check(host.resources() == 1 && host.references() == 2, "revoked notification leaked support");
                parent.notifications = child.notifications = notices;
                parent.references = child.references = refs;
                parent.child = &child; parent.probe = probe; parent.probe_user = &observed;
                check(notices->subscribe(request, callback, &parent, &parent.registration, nullptr) == DFY_V2_OK && !parent.calls,
                    "subscribe ran inline");
            }
            FifoExecutor executor(1);
            check(host.schedule_one(executor) == ScheduleResult::idle, "Pending request scheduled");
            queue->close(dfy::events::CloseMode::cancel); host.advance();
            check(host.schedule_one(executor) == ScheduleResult::queued && !parent.calls, "schedule ran inline");
            check(executor.run_one() == RunResult::executed && parent.calls == 1 && !child.calls && parent.revoke_status == DFY_V2_BUSY &&
                parent.retain_status == DFY_V2_OK && observed.active == 1 && observed.result == dfy::platform::ModuleClose::busy && !observed.shutdown,
                "C callback missing context or code protection");
            check(host.schedule_one(executor) == ScheduleResult::queued && executor.run_one() == RunResult::executed && child.calls == 1,
                "child notification was lost");
            {
                auto binding = host.bind();
                refs->release(parent.registration, nullptr); refs->release(child.registration, nullptr);
                refs->release(parent.retained, nullptr); refs->release(child.retained, nullptr);
                dfy_test_notify_context revoked{};
                check(notices->subscribe(request, callback, &revoked, &revoked.registration, nullptr) == DFY_V2_OK &&
                    host.schedule_one(executor) == ScheduleResult::queued && notices->revoke(revoked.registration, nullptr) == DFY_V2_OK,
                    "queued revoke failed");
                refs->release(revoked.registration, nullptr);
                check(executor.run_one() == RunResult::executed && !revoked.calls, "revoked callback entered DLL");
            }
            dfy_test_notify_context orphan{};
            orphan.notifications = notices; orphan.references = refs;
            {
                auto binding = host.bind();
                check(notices->subscribe(request, callback, &orphan, &orphan.registration, nullptr) == DFY_V2_OK, "orphan subscribe failed");
                refs->release(orphan.registration, nullptr); refs->release(request, nullptr);
            }
            check(host.schedule_one(executor) == ScheduleResult::queued && executor.run_one() == RunResult::executed &&
                orphan.calls == 1 && orphan.retain_status == DFY_V2_OK, "client release cancelled notification");
            {
                auto binding = host.bind();
                dfy_v2_read_result result{};
                check(service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).result(orphan.retained, &result, nullptr) == DFY_V2_OK &&
                    result.outcome == DFY_V2_READ_CLOSED, "callback lost complete request");
                refs->release(orphan.retained, nullptr); refs->release(receiver, nullptr);
            }
            host.advance(); host.schedule_one(executor);
            check(!host.references() && !host.resources() && !host.cleanup_failed() && !module.active_calls(), "notification cleanup incomplete");
            auto final_queue = std::make_shared<dfy::events::SubscriptionQueue>(store, 1);
            const auto final_reader = host.expose(std::make_shared<dfy::events::SubscriptionReader>(final_queue, 1));
            dfy_test_notify_context stopped{};
            {
                auto binding = host.bind();
                check(service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).next(final_reader, &request, nullptr) == DFY_V2_OK &&
                    notices->subscribe(request, callback, &stopped, &stopped.registration, nullptr) == DFY_V2_OK, "shutdown setup failed");
            }
            final_queue->close(dfy::events::CloseMode::cancel); host.advance();
            check(host.schedule_one(executor) == ScheduleResult::queued && host.begin_shutdown() && !host.resources(), "shutdown did not revoke queued work");
            check(host.finish_shutdown() && host.finish_shutdown() && !host.references() && !host.code_holds(), "final shutdown not complete");
            check(module.close() == dfy::platform::ModuleClose::closed && executor.run_one() == RunResult::executed && !stopped.calls,
                "queued wrapper entered unloaded DLL");
        }
        check(module.close() == dfy::platform::ModuleClose::closed, "module still in use after host exit");
        std::cout << "Production C notification checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
