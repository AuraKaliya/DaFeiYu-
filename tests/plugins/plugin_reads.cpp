#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_read_result) == 24 && offsetof(dfy_v2_read_result, event_sequence) == 16);
static_assert(sizeof(dfy_v2_reader_api) == 32 && offsetof(dfy_v2_reader_api, payload) == 24);
namespace {
using namespace dfy::plugins;
// 在读取、预算或类型化结果违约时结束验收。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 取得当前宿主的精确版本服务表，避免把查询失败当作空能力调用。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(), name.size()}, 1, &table, nullptr) == DFY_V2_OK && table, "host service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1, "host service layout mismatch");
    return *static_cast<const T*>(table);
}
// 以真实 C 消费者请求取消，证明取消先到不消耗队列事件。
// 步骤：创建读取与取消入口 → 遍历三种原因 → 保留事件并验证迟到取消 → 回收所有请求。
void verify_cancel(const dfy_v2_plugin_api& bootstrap) {
    const void* table = nullptr;
    check(bootstrap.query_interface({DFY_TEST_CANCEL_ID, sizeof(DFY_TEST_CANCEL_ID) - 1}, 1, &table, nullptr) == DFY_V2_OK,
        "cancel consumer missing");
    const auto* consumer = static_cast<const dfy_test_cancel_api*>(table);
    check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 &&
        consumer->request && consumer->read, "invalid cancel consumer");
    auto store = std::make_shared<dfy::events::EventStore>(4, 0);
    auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store, 1);
    auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue, 2);
    HostServices host(4);
    const auto receiver = host.expose(reader);
    auto binding = host.bind();
    const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
    const auto& cancels = service<dfy_v2_operation_cancel_api>(DFY_V2_OPERATION_CANCEL_INTERFACE);
    const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
    auto descriptor = std::make_shared<const dfy::runtime::StructDescriptor>(dfy::runtime::StructDescriptor::create("test.cancel", 1, {}, {0, 32}));
    auto payload = std::make_shared<const dfy::runtime::StructValue>(dfy::runtime::StructValue::create(descriptor, {}, 0));
    for (auto reason : {DFY_V2_CANCEL_USER, DFY_V2_CANCEL_OWNER_STOP, DFY_V2_CANCEL_DEADLINE}) {
        dfy_v2_handle request{}, delivered{}, rejected{};
        uint32_t disposition = 99;
        dfy_v2_cancel_snapshot cancel{};
        dfy_v2_read_result result{};
        check(reads.next(receiver, &request, nullptr) == DFY_V2_OK, "cancel read not accepted");
        check(consumer->request(&cancels, request, 0, &disposition, nullptr) == DFY_V2_INVALID_ARGUMENT && !disposition,
            "invalid reason applied");
        check(consumer->request(&cancels, receiver, reason, &disposition, nullptr) == DFY_V2_INVALID_ARGUMENT && !disposition,
            "reader was cancelled as operation");
        const auto event = store->accept(payload); queue->push(event); store->release(event);
        check(consumer->request(&cancels, request, reason, &disposition, nullptr) == DFY_V2_OK && disposition == DFY_V2_CANCEL_RECORDED,
            "cancel not recorded");
        check(consumer->request(&cancels, request, reason % 3 + 1, &disposition, nullptr) == DFY_V2_OK &&
            disposition == DFY_V2_CANCEL_ALREADY_REQUESTED && consumer->read(&cancels, request, &cancel, nullptr) == DFY_V2_OK &&
            cancel.reason == reason, "first reason changed");
        host.advance();
        check(reads.result(request, &result, nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_CANCELLED &&
            !result.close_mode && !result.event_sequence && queue->size() == 1, "cancel consumed event or closed stream");
        check(reads.next(receiver, &delivered, nullptr) == DFY_V2_OK &&
            consumer->request(&cancels, delivered, reason, &disposition, nullptr) == DFY_V2_OK &&
            disposition == DFY_V2_CANCEL_ALREADY_SETTLED, "late cancel changed result");
        check(reads.next(receiver, &rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && host.references() == 3,
            "result capacity failure leaked placeholder");
        refs.release(request, nullptr); refs.release(delivered, nullptr); host.advance();
        check(!store->references() && !reader->size(), "cancelled or delivered request leaked");
    }
    reader->close(dfy::events::CloseMode::cancel); host.advance();
    refs.release(receiver, nullptr);
}
}
// 通过真实 C DLL 提交生产读取，并验证结果与事件预算不能随状态引用释放而逃逸。
// 步骤：建立有界宿主 → 提交与拒绝重复读取 → 交付事件并保留载荷 → 释放控制引用后关闭及回收。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected C fixture DLL");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap, "query failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_READS_ID, sizeof(DFY_TEST_READS_ID) - 1}, 1, &table, nullptr) == DFY_V2_OK,
            "reader consumer missing");
        const auto* consumer = static_cast<const dfy_test_reads_api*>(table);
        check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 &&
            consumer->next && consumer->result, "invalid reader consumer");
        auto store = std::make_shared<dfy::events::EventStore>(4, 0);
        auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store, 2);
        auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue, 2);
        const std::weak_ptr<dfy::events::SubscriptionReader> reader_lifetime = reader;
        HostServices host(4);
        const auto receiver = host.expose(reader);
        reader.reset();
        auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
        const auto& states = service<dfy_v2_operation_state_api>(DFY_V2_OPERATION_STATE_INTERFACE);
        dfy_v2_handle request{}, rejected{}, body{};
        dfy_v2_read_result result{};
        dfy_v2_operation_snapshot state{};
        check(refs.retain(receiver, &rejected, nullptr) == DFY_V2_UNSUPPORTED, "reader copied");
        check(consumer->next(HostServices::query, receiver, &request, nullptr) == DFY_V2_OK && host.references() == 2,
            "read submission failed");
        check(consumer->next(HostServices::query, receiver, &rejected, nullptr) == DFY_V2_BUSY && !rejected.id && host.references() == 2,
            "duplicate pending left reference");
        check(consumer->result(HostServices::query, request, &result, nullptr) == DFY_V2_BUSY && !result.outcome &&
            states.read(request, &state, nullptr) == DFY_V2_OK && state.state == DFY_V2_OPERATION_PENDING, "pending result wrong");
        auto descriptor = std::make_shared<const dfy::runtime::StructDescriptor>(
            dfy::runtime::StructDescriptor::create("test.event", 1, {}, {0, 32}));
        auto value = std::make_shared<const dfy::runtime::StructValue>(dfy::runtime::StructValue::create(descriptor, {}, 0));
        const auto event = store->accept(value);
        queue->push(event); store->release(event);
        host.advance();
        check(consumer->result(HostServices::query, request, &result, nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_EVENT &&
            result.event_domain == store->domain() && result.event_sequence == 1 && !result.close_mode, "event result changed");
        const auto filler = host.expose(std::make_shared<const dfy::runtime::Value>(dfy::runtime::Value::from_bool(true)));
        const auto filler2 = host.expose(std::make_shared<const dfy::runtime::Value>(dfy::runtime::Value::from_bool(false)));
        check(reads.payload(request, &body, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !body.id && host.references() == 4,
            "payload capacity failure leaked reference");
        refs.release(filler, nullptr); refs.release(filler2, nullptr);
        check(reads.payload(request, &body, nullptr) == DFY_V2_OK && refs.release(request, nullptr) == DFY_V2_OK, "payload hold failed");
        host.advance();
        check(store->references() == 1 && reader_lifetime.lock()->size() == 1, "payload escaped event or result budget");
        check(states.read(request, &state, nullptr) == DFY_V2_INVALID_HANDLE && !state.state, "released request accepted");
        refs.release(body, nullptr); host.advance();
        check(!store->references() && !reader_lifetime.lock()->size(), "result storage leaked");
        check(consumer->next(HostServices::query, receiver, &request, nullptr) == DFY_V2_OK, "second read failed");
        refs.release(receiver, nullptr);
        queue->close(dfy::events::CloseMode::cancel); host.advance();
        check(consumer->result(HostServices::query, request, &result, nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_CLOSED &&
            result.close_mode == DFY_V2_CLOSE_CANCEL && !result.event_domain && !result.event_sequence, "host lost read or closed result");
        check(reads.payload(request, &body, nullptr) == DFY_V2_INVALID_ARGUMENT && !body.id, "closed read returned payload");
        refs.release(request, nullptr); host.advance();
        check(!host.references() && !store->references(), "read references leaked");
        verify_cancel(*bootstrap);
        std::cout << "Production C read checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
