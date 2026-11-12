#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <thread>
#include <cstddef>
static_assert(sizeof(dfy_v2_type_info) == 32 && alignof(dfy_v2_type_info) == 8 && offsetof(dfy_v2_type_info, field_count) == 24);
static_assert(sizeof(dfy_v2_field_info) == 32 && alignof(dfy_v2_field_info) == 8 && offsetof(dfy_v2_field_info, max_bytes) == 24);
static_assert(sizeof(dfy_v2_type_api) == 24 && offsetof(dfy_v2_type_api, field) == 16);

namespace {
using namespace dfy::plugins;
// 在服务绑定、引用或边界错误不符合契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证纯 C 对未预编译结构的枚举及宿主描述引用寿命。
// 步骤：建立六种字段的描述 → 查询并消费反射服务 → 拒绝索引及种类错误 → 保留后重新借用并回收。
void verify_types(const dfy_v2_plugin_api& bootstrap) {
    using namespace dfy::runtime;
    const FieldDescriptor fields[] = {{"a", ValueKind::boolean, 0}, {"b", ValueKind::int64, 0},
        {"c", ValueKind::uint64, 0}, {"d", ValueKind::float64, 0}, {"e", ValueKind::string, 8}, {"f", ValueKind::bytes, 9}};
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create("uncompiled.record", UINT32_MAX, fields, {6, 64}));
    const std::weak_ptr<const StructDescriptor> lifetime = descriptor;
    HostServices host(3);
    const auto original = host.expose(descriptor);
    descriptor.reset();
    auto binding = host.bind();
    const void* table = nullptr;
    check(bootstrap.query_interface({DFY_TEST_TYPES_ID, sizeof(DFY_TEST_TYPES_ID) - 1}, 1, &table, nullptr) == DFY_V2_OK,
        "missing reflection consumer");
    const auto* consumer = static_cast<const dfy_test_types_api*>(table);
    check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 &&
        consumer->inspect, "invalid reflection consumer");
    uint32_t kinds = 0;
    check(consumer->inspect(HostServices::query, original, &kinds, nullptr) == DFY_V2_OK && kinds == 126, "C reflection failed");
    check(HostServices::query({DFY_V2_TYPE_INTERFACE, sizeof(DFY_V2_TYPE_INTERFACE) - 1}, 1, &table, nullptr) == DFY_V2_OK,
        "missing type service");
    const auto* types = static_cast<const dfy_v2_type_api*>(table);
    dfy_v2_type_info info{};
    dfy_v2_field_info field{};
    check(types->describe(original, &info, nullptr) == DFY_V2_OK && info.contract_version == UINT32_MAX &&
        info.field_count == 6 && std::string_view(info.name.data, info.name.length) == "uncompiled.record", "type metadata lost");
    check(types->field(original, 5, &field, nullptr) == DFY_V2_OK && field.kind == DFY_V2_VALUE_BYTES && field.max_bytes == 9,
        "field order or constraint changed");
    check(types->field(original, UINT64_MAX, &field, nullptr) == DFY_V2_INVALID_ARGUMENT && !field.name.data && !field.kind,
        "invalid index returned field");
    check(types->describe(original, nullptr, nullptr) == DFY_V2_INVALID_ARGUMENT, "null type output accepted");
    const auto value = host.expose(std::make_shared<const Value>(Value::from_bool(true)));
    check(types->describe(value, &info, nullptr) == DFY_V2_INVALID_ARGUMENT && !info.name.data && !info.field_count,
        "wrong object kind returned metadata");
    check(HostServices::query({DFY_V2_REFERENCE_INTERFACE, sizeof(DFY_V2_REFERENCE_INTERFACE) - 1}, 1, &table, nullptr) == DFY_V2_OK,
        "missing references");
    const auto* refs = static_cast<const dfy_v2_reference_api*>(table);
    dfy_v2_handle owned{};
    check(refs->retain(original, &owned, nullptr) == DFY_V2_OK && refs->release(original, nullptr) == DFY_V2_OK,
        "descriptor retain failed");
    check(types->describe(original, &info, nullptr) == DFY_V2_INVALID_HANDLE && !info.name.data, "stale descriptor accepted");
    check(consumer->inspect(HostServices::query, owned, &kinds, nullptr) == DFY_V2_OK && kinds == 126, "retained descriptor lost");
    refs->release(owned, nullptr); refs->release(value, nullptr);
    check(lifetime.expired() && !host.references(), "description reference leaked");
}
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
        verify_types(*bootstrap);
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
