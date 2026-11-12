#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <thread>

namespace {
using namespace dfy::plugins;
// 在服务绑定、引用或边界错误不符合契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
}

// 用真实 C DLL 消费生产宿主引用服务，验证实例隔离和调用期绑定。
// 步骤：查询 C 消费能力 → 拒绝无上下文调用 → 验证独立引用及嵌套隔离 → 卸载消费 DLL 后回收宿主数据。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected C consumer DLL");
        dfy::platform::DynamicLibrary library(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(library.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap, "query failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_HOST_ID, sizeof(DFY_TEST_HOST_ID) - 1}, 1, &table, nullptr) ==
            DFY_V2_OK && table, "host consumer missing");
        const auto* consumer = static_cast<const dfy_test_host_api*>(table);
        check(consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 &&
            consumer->preserve, "invalid host consumer");
        HostServices host(2), other(1);
        auto value = std::make_shared<const dfy::runtime::Value>(dfy::runtime::Value::from_int64(42));
        const std::weak_ptr<const dfy::runtime::Value> lifetime = value;
        const auto source = host.expose(value);
        value.reset();
        dfy_v2_handle owned{}, extra{};
        const dfy_v2_string_view name{DFY_V2_REFERENCE_INTERFACE, sizeof(DFY_V2_REFERENCE_INTERFACE) - 1};
        check(consumer->preserve(HostServices::query, source, &owned, nullptr) == DFY_V2_INVALID_CONTEXT && !owned.id,
            "unbound DLL service call accepted");
        const dfy_v2_reference_api* references = nullptr;
        {
            auto binding = host.bind();
            dfy_v2_error error{}; error.length = 4;
            check(HostServices::query(name, 1, &table, &error) == DFY_V2_OK && !error.length, "host query failed");
            references = static_cast<const dfy_v2_reference_api*>(table);
            check(consumer->preserve(HostServices::query, source, &owned, &error) == DFY_V2_OK && !error.length &&
                owned.id != source.id && host.references() == 2, "C consumer did not retain independently");
            check(references->retain(source, &extra, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !extra.id,
                "reference capacity not mapped");
            {
                auto nested = other.bind();
                check(references->release(source, nullptr) == DFY_V2_INVALID_HANDLE && host.references() == 2,
                    "foreign context released source");
            }
            check(references->release(source, nullptr) == DFY_V2_OK && !lifetime.expired(), "outer binding not restored");
            check(references->release(source, nullptr) == DFY_V2_INVALID_HANDLE, "stale source accepted");
            check(HostServices::query(name, 2, &table, nullptr) == DFY_V2_UNSUPPORTED && !table, "wrong version accepted");
            check(HostServices::query(name, 0, &table, nullptr) == DFY_V2_INVALID_ARGUMENT && !table, "zero version accepted");
            uint32_t thread_status = 0;
            bool wrong_thread = false;
            std::jthread worker([&] {
                const void* local = nullptr;
                thread_status = HostServices::query(name, 1, &local, nullptr);
                try { auto invalid = host.bind(); } catch (const std::logic_error&) { wrong_thread = true; }
            });
            worker.join();
            check(thread_status == DFY_V2_INVALID_CONTEXT && wrong_thread, "thread context leaked");
        }
        check(references->release(owned, nullptr) == DFY_V2_INVALID_CONTEXT && !lifetime.expired(), "scope leaked binding");
        library.close();
        {
            auto binding = host.bind();
            check(references->release(owned, nullptr) == DFY_V2_OK && lifetime.expired() && !host.references(),
                "host data depended on DLL or leaked");
            const auto exclusive = host.expose(std::make_shared<const dfy::runtime::Value>(dfy::runtime::Value::from_bool(true)), false);
            check(references->retain(exclusive, &extra, nullptr) == DFY_V2_UNSUPPORTED && !extra.id,
                "nonshareable reference accepted");
            check(references->release(exclusive, nullptr) == DFY_V2_OK && references->release({}, nullptr) == DFY_V2_OK,
                "release failed");
        }
        std::cout << "Production host reference checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
