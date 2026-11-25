#include <dafeiyu/host_services.hpp>
#include <dafeiyu/processing_abi_v2.h>
#include "fixtures/query_v2/api.h"
#include <iostream>
namespace {
using namespace dfy::runtime;
using namespace dfy::events;
using namespace dfy::plugins;
// 在真实 C 处理、数据寿命或代码保留违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 查询已校验公共大小的宿主服务。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(),name.size()},1,&table,nullptr) == DFY_V2_OK && table,"processing DLL host service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1,"processing DLL host layout wrong");
    return *static_cast<const T*>(table);
}
// 在固定预算满时确认宿主拒绝，不接受其他失败类别。
void full(HostServices& host, const EventLease& data) {
    try { (void)host.submit_processing(data); } catch (const std::length_error&) { return; }
    throw std::runtime_error("retained processing payload bypassed capacity");
}
}
// 由真实 C DLL 分两次确认结果和退出，并在跨配置宿主中验证其独立寿命。
// 步骤：受控加载夹具 → 接纳和延后授予 → 保留结果并拒绝忙碌停止 → 确认退出及预算 → 停止并卸载。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2,"processing DLL path required");
        dfy::platform::CodeModule module(std::filesystem::canonical(argv[1]),8);
        auto code = module.lease(); const dfy_test_processing_api* consumer = nullptr;
        {
            auto call = code.enter(); const dfy_v2_plugin_api* bootstrap = nullptr; const void* table = nullptr;
            const auto query = reinterpret_cast<dfy_v2_query_fn>(call.symbol(DFY_V2_QUERY_EXPORT));
            check(query(2,&bootstrap,nullptr) == DFY_V2_OK && bootstrap && bootstrap->query_interface(
                {DFY_TEST_PROCESSING_ID,sizeof(DFY_TEST_PROCESSING_ID)-1},1,&table,nullptr) == DFY_V2_OK && table,"processing DLL query failed");
            consumer = static_cast<const dfy_test_processing_api*>(table);
            check(consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 && consumer->step,"processing DLL layout wrong");
        }
        auto store = std::make_shared<EventStore>(6,0); SubscriptionQueue receiver(store,1);
        const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.processing_dll",1,{}, {0,32}));
        const auto event = store->accept(std::make_shared<const StructValue>(StructValue::create(type,{},0)));
        receiver.push(event); store->release(event); auto data = receiver.poll().event;
        HostServices host(8); host.enable_processing(1); host.enable_notifications(module.lease(),1);
        auto work = host.submit_processing(data); dfy_v2_handle body{};
        {
            auto binding = host.bind(); auto call = code.enter();
            check(consumer->step(HostServices::query,work,0,&body,nullptr) == DFY_V2_BUSY && !body.id,"C processing started inline");
        }
        host.advance();
        {
            auto binding = host.bind(); auto call = code.enter();
            check(consumer->step(HostServices::query,work,0,&body,nullptr) == DFY_V2_OK && body.id && !host.begin_shutdown(),"C result completed actual exit");
            dfy_v2_processing_info info{};
            check(service<dfy_v2_processing_api>(DFY_V2_PROCESSING_INTERFACE).read(work,&info,nullptr) == DFY_V2_OK &&
                info.result == DFY_V2_OPERATION_SUCCEEDED && info.exit == DFY_V2_OPERATION_PENDING,"C result and exit collapsed");
            dfy_v2_handle unused{};
            check(consumer->step(HostServices::query,work,1,&unused,nullptr) == DFY_V2_OK && !unused.id,"C actual exit failed");
            check(service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE).release(work,nullptr) == DFY_V2_OK,"C processing reference release failed");
        }
        host.advance(); full(host,data);
        {
            auto binding = host.bind(); const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
            dfy_v2_handle descriptor{}; dfy_v2_type_info info{};
            check(service<dfy_v2_struct_api>(DFY_V2_STRUCT_INTERFACE).descriptor(body,&descriptor,nullptr) == DFY_V2_OK &&
                service<dfy_v2_type_api>(DFY_V2_TYPE_INTERFACE).describe(descriptor,&info,nullptr) == DFY_V2_OK &&
                std::string_view(info.name.data,info.name.length) == "test.processing_dll","C exit invalidated retained payload");
            refs.release(descriptor,nullptr); refs.release(body,nullptr);
        }
        host.advance(); work = host.submit_processing(data);
        check(host.begin_shutdown() && host.finish_shutdown() && !host.references() && !host.resources(),"C processing cleanup leaked");
        check(module.close() == dfy::platform::ModuleClose::in_use,"explicit caller code lease lost");
        code.reset(); check(module.close() == dfy::platform::ModuleClose::closed,"processing DLL could not unload");
        data.reset(); check(!store->references(),"processing data survived final owner");
        std::cout << "Processing DLL checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
