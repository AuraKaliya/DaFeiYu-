#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace dfy::plugins;
// 在组读取隔离、兼容或停止责任违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 查询当前绑定的精确版本能力并校验布局。
// 步骤：查询版本 1 → 核对公共头 → 返回只读表。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"group service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1,"group service layout wrong");
    return *static_cast<const T*>(table);
}
// 通过既有纯 C 读取服务验收成员取消、结果预算和宿主独立停止。
// 步骤：注入两个独立成员 → 取消先到并由另一成员领取 → 停止首个宿主 → 归还结果后继续领取并最终收尾。
void verify(const dfy_test_reads_api& consumer) {
    auto store = std::make_shared<EventStore>(4,0); auto channel = std::make_shared<BroadcastChannel>(store,2,2);
    const auto group = channel->subscribe_group(2); const auto observer = channel->subscribe();
    auto first_reader = std::make_shared<GroupReader>(group,1), second_reader = std::make_shared<GroupReader>(group,1);
    const std::weak_ptr<GroupReader> first_lifetime = first_reader, second_lifetime = second_reader;
    HostServices first(8), second(8);
    first.enable_events(channel,1); second.enable_events(channel,1);
    const auto first_receiver = first.expose(first_reader), second_receiver = second.expose(second_reader);
    bool duplicate = false;
    try { (void)first.expose(first_reader); } catch (const std::logic_error&) { duplicate = true; }
    check(duplicate && first.references() == 1 && group->member_count() == 2,"duplicate group control accepted");
    first_reader.reset(); second_reader.reset();
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.group",1,{}, {0,32}));
    const auto payload = std::make_shared<const StructValue>(StructValue::create(type,{},0));
    dfy_v2_handle first_request{}, second_request{}, body{}, rejected{}; dfy_v2_read_result result{};
    {
        auto binding = first.bind(); uint32_t disposition = 0; dfy_v2_subscription_info info{};
        check(consumer.next(HostServices::query,first_receiver,&first_request,nullptr) == DFY_V2_OK,"first group wait failed");
        const auto& subscriptions = service<dfy_v2_subscription_api>(DFY_V2_SUBSCRIPTION_INTERFACE);
        check(subscriptions.close(first_receiver,DFY_V2_CLOSE_CANCEL,nullptr) == DFY_V2_INVALID_ARGUMENT &&
            subscriptions.read(first_receiver,&info,nullptr) == DFY_V2_INVALID_ARGUMENT && !info.state && group->member_count() == 2,"old subscription controlled group member");
        check(service<dfy_v2_operation_cancel_api>(DFY_V2_OPERATION_CANCEL_INTERFACE).request(first_request,DFY_V2_CANCEL_USER,&disposition,nullptr) == DFY_V2_OK,
            "group cancellation failed");
    }
    {
        auto binding = second.bind();
        check(consumer.next(HostServices::query,second_receiver,&second_request,nullptr) == DFY_V2_OK,"second group wait failed");
    }
    const auto id = channel->publish(payload); first.advance(); second.advance();
    check(observer->poll().event.get().id == id,"ordinary broadcast disagreed with group event");
    {
        auto binding = first.bind();
        check(consumer.result(HostServices::query,first_request,&result,nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_CANCELLED && !result.event_sequence,"cancelled member claimed event");
        service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE).release(first_request,nullptr); first.advance();
        check(consumer.next(HostServices::query,first_receiver,&first_request,nullptr) == DFY_V2_OK,"member could not wait again");
    }
    {
        auto binding = second.bind();
        check(consumer.result(HostServices::query,second_request,&result,nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_EVENT && result.event_sequence == id.sequence,
            "group delivery duplicated or lost");
        check(service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).payload(second_request,&body,nullptr) == DFY_V2_OK,"group result hold failed");
        service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE).release(second_request,nullptr); second.advance();
    }
    check(first.begin_shutdown() && !first.resources() && group->member_count() == 1 && group->state() == QueueState::open,"host stop closed shared group");
    {
        auto binding = first.bind();
        check(consumer.result(HostServices::query,first_request,&result,nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_CLOSED &&
            result.close_mode == DFY_V2_CLOSE_CANCEL && consumer.next(HostServices::query,first_receiver,&rejected,nullptr) == DFY_V2_BUSY && !rejected.id,
            "stopped member retained admission or wrong end result");
    }
    check(first.finish_shutdown() && first_lifetime.expired(),"stopped member control leaked");
    const auto next = channel->publish_to_group(payload,group);
    {
        auto binding = second.bind();
        check(consumer.next(HostServices::query,second_receiver,&rejected,nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && group->queued() == 1,
            "group result budget was bypassed");
        service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE).release(body,nullptr); second.advance();
        check(consumer.next(HostServices::query,second_receiver,&second_request,nullptr) == DFY_V2_OK &&
            consumer.result(HostServices::query,second_request,&result,nullptr) == DFY_V2_OK && result.event_sequence == next.sequence,"remaining member did not continue");
    }
    check(second.begin_shutdown() && !group->member_count() && group->state() == QueueState::open && store->references() == 1,"group stop lost retained result");
    {
        auto binding = second.bind();
        check(consumer.result(HostServices::query,second_request,&result,nullptr) == DFY_V2_OK && result.event_sequence == next.sequence,"shutdown rewrote successful read");
    }
    check(second.finish_shutdown() && second_lifetime.expired() && !store->references(),"group result or control leaked");
    group->close(CloseMode::cancel); channel->remove_group(group); observer->close(CloseMode::cancel); channel->remove(observer);
    check(!channel->size() && !first.references() && !second.references(),"group host references leaked");
}
}
// 以真实 C DLL 验收宿主注入的组成员接收方，不依赖新的组创建 ABI。
// 步骤：加载读取夹具 → 校验能力表 → 执行成员隔离及收尾验收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"C group read fixture required");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap,"group read bootstrap failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_READS_ID,sizeof(DFY_TEST_READS_ID)-1},1,&table,nullptr) == DFY_V2_OK && table,"group read consumer missing");
        const auto& consumer = *static_cast<const dfy_test_reads_api*>(table);
        check(consumer.header.struct_size == sizeof(consumer) && consumer.header.interface_version == 1 && consumer.next && consumer.result,"invalid group read consumer");
        verify(consumer); std::cout << "C group member read checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
