#include <dafeiyu/host_services.hpp>
#include <dafeiyu/processing_abi_v2.h>
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_processing_info) == 48 && offsetof(dfy_v2_processing_info,order_domain) == 32);
static_assert(sizeof(dfy_v2_processing_api) == 56 && offsetof(dfy_v2_processing_api,payload) == 48);
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace dfy::plugins;
// 在处理 ABI、预算或停止边界违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 查询精确版本布局，禁止读取未知服务表。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"processing service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1,"processing service layout wrong");
    return *static_cast<const T*>(table);
}
}
// 验证真实宿主处理协议，结果已确定但尚未退出时卸载仍无副作用地拒绝。
// 步骤：提交真实事件 → 核对顺序与错误上下文 → 结算业务并拒绝忙碌停止 → 实际退出后取消等待并关闭。
int main() {
    try {
        auto store = std::make_shared<EventStore>(8,0); SubscriptionQueue receiver(store,1);
        const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.work",1,{}, {0,32}));
        const auto event = store->accept(std::make_shared<const StructValue>(StructValue::create(type,{},0)));
        receiver.push(event); store->release(event); auto data = receiver.poll().event;
        HostServices host(8), other(4); host.enable_processing(2); other.enable_processing(1);
        const auto first = host.submit_processing(data), next = host.submit_processing(data); auto binding = host.bind();
        const auto& api = service<dfy_v2_processing_api>(DFY_V2_PROCESSING_INTERFACE);
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        dfy_v2_processing_info info{}; dfy_v2_handle retained{}; uint32_t disposition = 0;
        check(api.start(first,nullptr) == DFY_V2_BUSY && api.read(first,&info,nullptr) == DFY_V2_OK && info.order_sequence == 1 &&
            info.event_sequence == 1 && info.phase == DFY_V2_PROCESSING_WAITING,"processing accepted or started inline");
        check(refs.retain(first,&retained,nullptr) == DFY_V2_UNSUPPORTED && !retained.id,"completion authority was copied");
        { auto scope = other.bind(); check(api.start(first,nullptr) == DFY_V2_INVALID_HANDLE && api.read(first,&info,nullptr) == DFY_V2_INVALID_HANDLE && !info.phase,"foreign host controlled processing"); }
        host.advance(); check(api.start(first,nullptr) == DFY_V2_OK && host.processing_busy(),"granted processing failed to start");
        check(api.finish(first,nullptr) == DFY_V2_FAILED && api.complete(first,99,nullptr) == DFY_V2_INVALID_ARGUMENT,"invalid finish or result accepted");
        check(api.complete(first,DFY_V2_OPERATION_SUCCEEDED,nullptr) == DFY_V2_OK && !host.begin_shutdown() &&
            api.read(next,&info,nullptr) == DFY_V2_OK && !info.cancel_reason,"busy shutdown changed pending work");
        check(api.cancel(first,DFY_V2_CANCEL_USER,&disposition,nullptr) == DFY_V2_OK && disposition == DFY_V2_CANCEL_RECORDED &&
            api.read(first,&info,nullptr) == DFY_V2_OK && info.result == DFY_V2_OPERATION_SUCCEEDED && info.exit == DFY_V2_OPERATION_PENDING,
            "cancel overwrote result or forged exit");
        check(api.payload(first,&retained,nullptr) == DFY_V2_OK && api.finish(first,nullptr) == DFY_V2_OK && !host.processing_busy(),"payload or actual exit failed");
        check(api.finish(first,nullptr) == DFY_V2_FAILED && host.begin_shutdown() && !host.resources() &&
            api.read(next,&info,nullptr) == DFY_V2_OK && info.result == DFY_V2_OPERATION_CANCELLED && info.exit == DFY_V2_OPERATION_SUCCEEDED,
            "shutdown did not cancel unstarted processing");
        check(refs.release(first,nullptr) == DFY_V2_OK && refs.release(next,nullptr) == DFY_V2_OK && refs.release(retained,nullptr) == DFY_V2_OK &&
            host.finish_shutdown() && !host.references(),"processing shutdown leaked resources");
        check(api.start(first,nullptr) == DFY_V2_UNSUPPORTED && other.begin_shutdown() && other.finish_shutdown(),"cached capability dereferenced closed queue");
        std::cout << "Plugin processing checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
