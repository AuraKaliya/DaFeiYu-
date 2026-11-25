#include <dafeiyu/host_services.hpp>
#include <dafeiyu/group_abi_v2.h>
#include "fixtures/query_v2/api.h"
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace dfy::plugins;
// 在组通知的活动保护、绑定或资源回收违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 查询当前绑定的精确版本服务。
// 步骤：取得能力 → 核对公共布局 → 返回只读表。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"group notification service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1,"group notification service layout wrong");
    return *static_cast<const T*>(table);
}
struct Probe {
    dfy::platform::CodeModule* module;
    HostServices* host;
    const dfy_v2_group_api* groups;
    const dfy_v2_reference_api* refs;
    dfy_v2_handle receiver;
    std::size_t active = 0;
    dfy::platform::ModuleClose close = dfy::platform::ModuleClose::closed;
    uint32_t leave = DFY_V2_FAILED, join = DFY_V2_FAILED;
    bool stopped = true, failed = false;
};
// 在真实 C 栈内退出成员并尝试停止及关闭，任何异常仅记录而不跨 ABI。
// 步骤：记录活动并退出成员 → 尝试关闭代码及宿主 → 验证拒绝后仍可接纳 → 归还临时成员。
void DFY_V2_CALL probe(void* user) noexcept {
    auto& value = *static_cast<Probe*>(user);
    try {
        value.active = value.module->active_calls();
        value.leave = value.groups->leave(value.receiver,nullptr);
        value.close = value.module->close(); value.stopped = value.host->begin_shutdown();
        dfy_v2_handle temporary{};
        value.join = value.groups->join(&temporary,nullptr);
        if (!value.join) {
            if (value.groups->leave(temporary,nullptr) || value.refs->release(temporary,nullptr)) value.failed = true;
        }
    } catch (...) { value.failed = true; }
}
}
// 验收组读取的真实 C 完成通知及代码、宿主和成员三类独立寿命。
// 步骤：查询受控 C 回调 → 延后通知并在活动栈内退出成员 → 读取保留结果 → 撤销排队回调并卸载。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"C group notification fixture required");
        dfy::platform::CodeModule module(std::filesystem::canonical(argv[1]),8);
        dfy_v2_completion_fn callback = nullptr;
        {
            auto code = module.lease(); auto call = code.enter();
            const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
            const dfy_v2_plugin_api* bootstrap = nullptr;
            check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap,"group notification bootstrap failed");
            const void* table = nullptr;
            check(bootstrap->query_interface({DFY_TEST_NOTIFY_ID,sizeof(DFY_TEST_NOTIFY_ID)-1},1,&table,nullptr) == DFY_V2_OK && table,"C group callback missing");
            const auto& api = *static_cast<const dfy_test_notify_api*>(table);
            check(api.header.struct_size == sizeof(api) && api.header.interface_version == 1 && api.callback,"C group callback invalid");
            callback = api.callback;
        }
        auto store = std::make_shared<EventStore>(4,0); auto group = std::make_shared<ConsumerGroup>(store,1,2);
        HostServices host(16); host.enable_group(group,2); host.enable_notifications(module.lease(),4);
        const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.group_notice",1,{}, {0,32}));
        const auto payload = std::make_shared<const StructValue>(StructValue::create(type,{},0));
        const dfy_v2_group_api* groups = nullptr; const dfy_v2_reference_api* refs = nullptr;
        const dfy_v2_operation_notify_api* notices = nullptr;
        dfy_v2_handle receiver{},request{}; dfy_test_notify_context context{};
        Probe observed{&module,&host,nullptr,nullptr,{}};
        {
            auto binding = host.bind(); groups = &service<dfy_v2_group_api>(DFY_V2_GROUP_INTERFACE);
            refs = &service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
            notices = &service<dfy_v2_operation_notify_api>(DFY_V2_OPERATION_NOTIFY_INTERFACE);
            check(groups->join(&receiver,nullptr) == DFY_V2_OK && service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).next(receiver,&request,nullptr) == DFY_V2_OK,"group callback read setup failed");
            observed.groups = groups; observed.refs = refs; observed.receiver = receiver;
            context.notifications = notices; context.references = refs; context.probe = probe; context.probe_user = &observed;
            check(notices->subscribe(request,callback,&context,&context.registration,nullptr) == DFY_V2_OK && !context.calls,"group notify called inline");
        }
        FifoExecutor executor(1);
        check(host.schedule_one(executor) == ScheduleResult::idle,"pending group read scheduled");
        const auto id = group->publish(payload); host.advance();
        check(host.schedule_one(executor) == ScheduleResult::queued && !context.calls,"group scheduling entered callback inline");
        check(executor.run_one() == RunResult::executed && context.calls == 1 && context.revoke_status == DFY_V2_BUSY && context.retain_status == DFY_V2_OK &&
            observed.active == 1 && observed.leave == DFY_V2_OK && observed.close == dfy::platform::ModuleClose::busy && !observed.stopped &&
            observed.join == DFY_V2_OK && !observed.failed && !group->member_count() && group->state() == QueueState::open,"group callback lost activity or member isolation");
        check(module.close() == dfy::platform::ModuleClose::in_use,"member exit released host code hold");
        {
            auto binding = host.bind(); dfy_v2_read_result result{}; dfy_v2_group_member_info info{};
            check(service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).result(context.retained,&result,nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_EVENT &&
                result.event_sequence == id.sequence && groups->read(receiver,&info,nullptr) == DFY_V2_OK && info.state == DFY_V2_GROUP_MEMBER_CLOSED,"callback exit invalidated retained result");
            refs->release(context.registration,nullptr); refs->release(context.retained,nullptr); refs->release(request,nullptr); refs->release(receiver,nullptr);
        }
        host.advance(); check(!host.resources() && !host.references() && !store->references(),"group callback retained native responsibility");
        dfy_test_notify_context revoked{}; revoked.notifications = notices; revoked.references = refs;
        {
            auto binding = host.bind();
            check(groups->join(&receiver,nullptr) == DFY_V2_OK && service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).next(receiver,&request,nullptr) == DFY_V2_OK &&
                notices->subscribe(request,callback,&revoked,&revoked.registration,nullptr) == DFY_V2_OK,"group revoke setup failed");
        }
        group->publish(payload); host.advance();
        check(host.schedule_one(executor) == ScheduleResult::queued && host.begin_shutdown() && host.finish_shutdown() &&
            !group->member_count() && group->state() == QueueState::open && !store->references(),"group stop did not revoke queued work");
        check(module.close() == dfy::platform::ModuleClose::closed && executor.run_one() == RunResult::executed && !revoked.calls &&
            executor.run_one() == RunResult::idle,"queued group wrapper entered unloaded DLL");
        group->close(CloseMode::cancel);
        std::cout << "C group notification checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
