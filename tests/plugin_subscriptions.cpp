#include <dafeiyu/host_services.hpp>
#include <dafeiyu/event_abi_v2.h>
#include <iostream>

namespace {
using namespace dfy::plugins;
// 在订阅接纳、关闭或清理不符合约定时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 读取当前绑定的精确版本服务，不复用实例地址。
template<class T>
const T& service(std::string_view name) {
    const void* table = nullptr;
    check(HostServices::query({name.data(), name.size()}, 1, &table, nullptr) == DFY_V2_OK && table, "service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == 1, "service mismatch");
    return *static_cast<const T*>(table);
}
}
// 验证 C 订阅服务的独立交付、两种关闭和创建失败回滚。
// 步骤：创建独立接收方 → 广播并分别关闭 → 验证流结束与数据寿命 → 注入引用预算不足并回收。
int main() {
    try {
        auto store = std::make_shared<dfy::events::EventStore>(8, 0);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store, 2, 2);
        HostServices host(6, 2);
        host.enable_events(channel, 2);
        auto binding = host.bind();
        const auto& subscriptions = service<dfy_v2_subscription_api>(DFY_V2_SUBSCRIPTION_INTERFACE);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE);
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE);
        dfy_v2_handle first{}, second{}, rejected{}, event{}, ended{};
        check(subscriptions.subscribe(&first, nullptr) == DFY_V2_OK && subscriptions.subscribe(&second, nullptr) == DFY_V2_OK,
            "subscription failed");
        check(subscriptions.subscribe(&rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id &&
            host.resources() == 2 && channel->size() == 2, "capacity left partial subscription");
        auto descriptor = std::make_shared<const dfy::runtime::StructDescriptor>(dfy::runtime::StructDescriptor::create("test.stream", 1, {}, {0, 32}));
        auto payload = std::make_shared<const dfy::runtime::StructValue>(dfy::runtime::StructValue::create(descriptor, {}, 0));
        channel->publish(payload);
        dfy_v2_subscription_info info{};
        check(subscriptions.close(first, 0, nullptr) == DFY_V2_INVALID_ARGUMENT && subscriptions.read(first, &info, nullptr) == DFY_V2_OK &&
            info.state == DFY_V2_SUBSCRIPTION_OPEN && info.queued == 1, "invalid close mutated queue");
        check(subscriptions.close(first, DFY_V2_CLOSE_DRAIN, nullptr) == DFY_V2_OK &&
            subscriptions.read(first, &info, nullptr) == DFY_V2_OK && info.state == DFY_V2_SUBSCRIPTION_DRAINING && info.queued == 1,
            "drain lost queued event");
        check(subscriptions.close(second, DFY_V2_CLOSE_CANCEL, nullptr) == DFY_V2_OK && channel->size() == 1 &&
            host.resources() == 1 && store->references() == 1, "cancel affected other receiver");
        check(reads.next(first, &event, nullptr) == DFY_V2_OK, "drain read failed");
        host.advance();
        check(!host.resources() && !channel->size() && store->references() == 1, "stream cleanup freed delivered data");
        check(reads.next(first, &ended, nullptr) == DFY_V2_OK, "closed read failed");
        dfy_v2_read_result result{};
        check(reads.result(ended, &result, nullptr) == DFY_V2_OK && result.outcome == DFY_V2_READ_CLOSED &&
            result.close_mode == DFY_V2_CLOSE_DRAIN, "end reason lost");
        check(subscriptions.close(first, DFY_V2_CLOSE_CANCEL, nullptr) == DFY_V2_OK &&
            subscriptions.read(first, &info, nullptr) == DFY_V2_OK && info.close_mode == DFY_V2_CLOSE_DRAIN, "closed mode rewritten");
        {
            HostServices full(1, 1);
            full.enable_events(channel, 1);
            const auto held = full.expose(std::make_shared<const dfy::runtime::Value>(dfy::runtime::Value::from_bool(true)));
            auto nested = full.bind();
            check(subscriptions.subscribe(&rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && !channel->size() &&
                !full.resources() && full.references() == 1 && !full.cleanup_failed(), "reference failure left managed work");
            refs.release(held, nullptr);
        }
        refs.release(first, nullptr); refs.release(second, nullptr); refs.release(event, nullptr); refs.release(ended, nullptr);
        check(!host.references() && !host.resources() && !store->references(), "subscription result leaked");
        std::cout << "Production C subscription checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
