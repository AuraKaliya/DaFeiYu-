#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <thread>
#include <cstddef>
static_assert(sizeof(dfy_v2_type_info) == 32 && alignof(dfy_v2_type_info) == 8 && offsetof(dfy_v2_type_info, field_count) == 24);
static_assert(sizeof(dfy_v2_field_info) == 32 && alignof(dfy_v2_field_info) == 8 && offsetof(dfy_v2_field_info, max_bytes) == 24);
static_assert(sizeof(dfy_v2_type_api) == 24 && offsetof(dfy_v2_type_api, field) == 16);
static_assert(sizeof(dfy_v2_value_data) == 16 && alignof(dfy_v2_value_data) == 8);
static_assert(sizeof(dfy_v2_value_view) == 24 && offsetof(dfy_v2_value_view, data) == 8);
static_assert(sizeof(dfy_v2_value_api) == 16 && sizeof(dfy_v2_struct_api) == 24);

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
// 验证纯 C 读取精确值，以及字段引用独立保持父载荷后的借用寿命。
// 步骤：构造六种值 → 由 C 消费者读取 → 验证错误与容量 → 释放父引用后重新借用字段并回收。
void verify_values(const dfy_v2_plugin_api& bootstrap) {
    using namespace dfy::runtime;
    const FieldDescriptor fields[] = {{"a", ValueKind::boolean, 0}, {"b", ValueKind::int64, 0},
        {"c", ValueKind::uint64, 0}, {"d", ValueKind::float64, 0}, {"e", ValueKind::string, 3}, {"f", ValueKind::bytes, 3}};
    auto descriptor = std::make_shared<const StructDescriptor>(StructDescriptor::create("other.record", 1, fields, {6, 64}));
    const uint8_t binary[]{0, 255, 128};
    const NamedValue input[] = {{"a", Value::from_bool(true)}, {"b", Value::from_int64(INT64_MIN)},
        {"c", Value::from_uint64(UINT64_MAX)}, {"d", Value::from_float64(1.25)},
        {"e", Value::from_string(std::string("A\0B", 3), 3)}, {"f", Value::from_bytes(binary, 3)}};
    auto payload = std::make_shared<const StructValue>(StructValue::create(descriptor, input, 31));
    const std::weak_ptr<const StructValue> lifetime = payload;
    descriptor.reset();
    HostServices host(4);
    const auto root = host.expose(payload);
    payload.reset();
    auto binding = host.bind();
    const void* table = nullptr;
    check(bootstrap.query_interface({DFY_TEST_VALUES_ID, sizeof(DFY_TEST_VALUES_ID) - 1}, 1, &table, nullptr) == DFY_V2_OK,
        "missing value consumer");
    const auto* consumer = static_cast<const dfy_test_values_api*>(table);
    check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 &&
        consumer->inspect && consumer->inspect(HostServices::query, root, nullptr) == DFY_V2_OK && host.references() == 1,
        "C value read or cleanup failed");
    HostServices::query({DFY_V2_STRUCT_INTERFACE, sizeof(DFY_V2_STRUCT_INTERFACE) - 1}, 1, &table, nullptr);
    const auto* structures = static_cast<const dfy_v2_struct_api*>(table);
    HostServices::query({DFY_V2_VALUE_INTERFACE, sizeof(DFY_V2_VALUE_INTERFACE) - 1}, 1, &table, nullptr);
    const auto* values = static_cast<const dfy_v2_value_api*>(table);
    HostServices::query({DFY_V2_REFERENCE_INTERFACE, sizeof(DFY_V2_REFERENCE_INTERFACE) - 1}, 1, &table, nullptr);
    const auto* refs = static_cast<const dfy_v2_reference_api*>(table);
    dfy_v2_handle type{}, field{}, retained{}, rejected{};
    dfy_v2_value_view view{};
    check(values->read(root, &view, nullptr) == DFY_V2_INVALID_ARGUMENT && !view.kind, "structure read as primitive");
    check(structures->field(root, UINT64_MAX, &rejected, nullptr) == DFY_V2_INVALID_ARGUMENT && !rejected.id, "field index accepted");
    check(structures->descriptor(root, &type, nullptr) == DFY_V2_OK && structures->field(root, 4, &field, nullptr) == DFY_V2_OK &&
        refs->retain(field, &retained, nullptr) == DFY_V2_OK, "independent field references failed");
    check(structures->field(root, 0, &rejected, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !rejected.id && host.references() == 4,
        "field capacity failure changed references");
    refs->release(root, nullptr); refs->release(field, nullptr);
    check(!lifetime.expired() && values->read(field, &view, nullptr) == DFY_V2_INVALID_HANDLE && !view.kind, "field identity reused");
    check(values->read(retained, &view, nullptr) == DFY_V2_OK && view.kind == DFY_V2_VALUE_STRING &&
        std::string_view(view.data.string.data, view.data.string.length) == std::string_view("A\0B", 3), "retained field lost payload");
    refs->release(retained, nullptr);
    check(lifetime.expired() && host.references() == 1, "descriptor unnecessarily held payload");
    refs->release(type, nullptr);
    check(!host.references(), "value references leaked");
}
// 旧版数据表对新结构种类明确返回不支持，不能输出占位成功或更改表布局。
void verify_nested_rejection() {
    using namespace dfy::runtime;
    auto leaf = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.leaf", 1, {}, {0, 16}));
    const FieldDescriptor field{"child", ValueKind::structure, 0, leaf};
    auto parent = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.parent", 1, {&field,1}, {1, 64, 2}));
    auto nested = std::make_shared<const StructValue>(StructValue::create(leaf, {}, 0));
    HostServices host(3); auto binding = host.bind();
    const auto type = host.expose(parent);
    const auto value = host.expose(std::make_shared<const Value>(Value::from_structure(nested)));
    const NamedValue child{"child", Value::from_structure(nested)};
    const auto structure = host.expose(std::make_shared<const StructValue>(StructValue::create(parent, {&child,1}, 0)));
    const void* table = nullptr;
    HostServices::query({DFY_V2_TYPE_INTERFACE, sizeof(DFY_V2_TYPE_INTERFACE)-1}, 1, &table, nullptr);
    dfy_v2_field_info info{};
    check(static_cast<const dfy_v2_type_api*>(table)->field(type, 0, &info, nullptr) == DFY_V2_UNSUPPORTED && !info.kind && !info.name.data,
        "v1 type profile accepted nested kind");
    HostServices::query({DFY_V2_VALUE_INTERFACE, sizeof(DFY_V2_VALUE_INTERFACE)-1}, 1, &table, nullptr);
    dfy_v2_value_view view{};
    check(static_cast<const dfy_v2_value_api*>(table)->read(value, &view, nullptr) == DFY_V2_UNSUPPORTED && !view.kind,
        "v1 value profile accepted nested kind");
    HostServices::query({DFY_V2_STRUCT_INTERFACE, sizeof(DFY_V2_STRUCT_INTERFACE)-1}, 1, &table, nullptr);
    dfy_v2_handle field_value{};
    check(static_cast<const dfy_v2_struct_api*>(table)->field(structure, 0, &field_value, nullptr) == DFY_V2_UNSUPPORTED && !field_value.id,
        "v1 structure profile returned unreadable nested field");
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
        verify_values(*bootstrap);
        verify_nested_rejection();
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
