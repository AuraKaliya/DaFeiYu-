#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace dfy::plugins;
// 在指定投递、实例隔离或数据寿命违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 查询当前宿主绑定中的精确版本 1 能力。
// 步骤：查询能力 → 校验公共布局 → 返回只读函数表。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"target host service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1,"target host layout mismatch");
    return *static_cast<const T*>(table);
}
// 以两个独立宿主上下文消费定向投递，非目标保留等待且不能读取其他域的请求。
// 步骤：建立独立等待与非目标背压 → 指定交付并核对隔离 → 移除目标后继续读取旧数据 → 混合广播并收尾。
void verify(const dfy_test_reads_api& consumer) {
    auto store = std::make_shared<EventStore>(4,0);
    BroadcastChannel channel(store,3,1);
    const auto first_queue = channel.subscribe(), second_queue = channel.subscribe(), other = channel.subscribe();
    const auto first_reader = std::make_shared<SubscriptionReader>(first_queue,1);
    const auto second_reader = std::make_shared<SubscriptionReader>(second_queue,1);
    HostServices first(8), second(8);
    const auto first_receiver = first.expose(first_reader), second_receiver = second.expose(second_reader);
    dfy_v2_handle first_request{}, second_request{}, body{}; dfy_v2_read_result result{};
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("tests.target",1,{}, {0,32}));
    const auto payload = std::make_shared<const StructValue>(StructValue::create(type,{},0));
    const auto occupied = channel.publish_to(payload,{&other,1});
    {
        auto binding = first.bind();
        check(consumer.next(HostServices::query,first_receiver,&first_request,nullptr) == DFY_V2_OK,"first C wait failed");
    }
    {
        auto binding = second.bind();
        check(consumer.next(HostServices::query,second_receiver,&second_request,nullptr) == DFY_V2_OK &&
            consumer.result(HostServices::query,first_request,&result,nullptr) == DFY_V2_INVALID_HANDLE && !result.outcome,"C target domains were mixed");
    }
    const auto first_id = channel.publish_to(payload,{&first_queue,1});
    {
        auto binding = first.bind();
        check(consumer.result(HostServices::query,first_request,&result,nullptr) == DFY_V2_BUSY && !result.outcome,"target admission advanced consumer inline");
    }
    first.advance(); second.advance();
    {
        auto binding = first.bind();
        check(consumer.result(HostServices::query,first_request,&result,nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_EVENT &&
            result.event_domain == first_id.domain && result.event_sequence == first_id.sequence,"first target received wrong event");
        check(service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE).payload(first_request,&body,nullptr) == DFY_V2_OK &&
            service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE).release(first_request,nullptr) == DFY_V2_OK,"target payload hold failed");
    }
    first.advance();
    {
        auto binding = second.bind();
        check(consumer.result(HostServices::query,second_request,&result,nullptr) == DFY_V2_BUSY && !result.outcome && other->size() == 1,
            "non-target consumed directed event or lost its queue");
    }
    check(first_reader->size() == 1 && store->references() == 2,"target payload escaped result budget");
    const auto second_id = channel.publish_to(payload,{&second_queue,1}); second.advance();
    {
        auto binding = second.bind();
        check(consumer.result(HostServices::query,second_request,&result,nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_EVENT &&
            result.event_domain == second_id.domain && result.event_sequence == second_id.sequence && second_id.sequence == first_id.sequence + 1,"second target result wrong");
    }
    first_reader->close(CloseMode::cancel); channel.remove(first_queue);
    bool rejected = false;
    try { (void)channel.publish_to(payload,{&first_queue,1}); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected && store->references() == 3,"removed target altered retained data");
    {
        auto binding = first.bind(); dfy_v2_handle descriptor{}; dfy_v2_type_info info{};
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        check(service<dfy_v2_struct_api>(DFY_V2_STRUCT_INTERFACE).descriptor(body,&descriptor,nullptr) == DFY_V2_OK &&
            service<dfy_v2_type_api>(DFY_V2_TYPE_INTERFACE).describe(descriptor,&info,nullptr) == DFY_V2_OK &&
            std::string_view(info.name.data,info.name.length) == "tests.target","target removal invalidated delivered data");
        refs.release(descriptor,nullptr); refs.release(body,nullptr); refs.release(first_receiver,nullptr);
    }
    first.advance(); check(!first.references() && !first.resources(),"first target cleanup retained responsibility");
    {
        auto binding = second.bind();
        service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE).release(second_request,nullptr);
        second.advance();
        check(consumer.next(HostServices::query,second_receiver,&second_request,nullptr) == DFY_V2_OK,"broadcast C wait failed");
    }
    check(other->poll().event.get().id == occupied,"non-target queued event changed");
    const auto broadcast = channel.publish(payload); second.advance();
    {
        auto binding = second.bind();
        check(consumer.result(HostServices::query,second_request,&result,nullptr) == DFY_V2_OK && result.event_sequence == broadcast.sequence &&
            broadcast.sequence == second_id.sequence + 1,"mixed C broadcast order changed");
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        refs.release(second_request,nullptr); refs.release(second_receiver,nullptr);
    }
    second_reader->close(CloseMode::cancel); second.advance(); channel.remove(second_queue);
    other->close(CloseMode::cancel); channel.remove(other);
    check(!channel.size() && !store->references() && !second.references() && !second.resources(),"C targeted delivery leaked resources");
}
}
// 使用真实 C DLL 验收两个宿主上下文的定向读取和广播共存。
// 步骤：加载查询夹具 → 校验读取能力 → 运行隔离及预算链验收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"C target consumer DLL required");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap,"C target bootstrap failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_READS_ID,sizeof(DFY_TEST_READS_ID)-1},1,&table,nullptr) == DFY_V2_OK && table,"C target consumer missing");
        const auto& consumer = *static_cast<const dfy_test_reads_api*>(table);
        check(consumer.header.struct_size == sizeof(consumer) && consumer.header.interface_version == 1 && consumer.next && consumer.result,"C target consumer invalid");
        verify(consumer); std::cout << "Real C targeted delivery checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
