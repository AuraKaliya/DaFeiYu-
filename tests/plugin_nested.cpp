#include <dafeiyu/host_services.hpp>
#include <dafeiyu/dynamic_library.hpp>
#include "fixtures/query_v2/api.h"
#include <iostream>
#include <cstddef>
static_assert(sizeof(dfy_v2_type_api_v2) == 32 && offsetof(dfy_v2_type_api_v2, nested) == 24);
static_assert(sizeof(dfy_v2_value_api_v2) == 24 && offsetof(dfy_v2_value_api_v2, structure) == 16);
static_assert(sizeof(dfy_v2_field_info_v2) == 32 && offsetof(dfy_v2_field_info_v2, flags) == 20);
namespace {
using namespace dfy::runtime;
using namespace dfy::plugins;
// 在版本化访问、递归读取或预算链违反契约时报告失败。
void check(bool condition, const char* reason) { if (!condition) throw std::runtime_error(reason); }
// 查询当前绑定的精确能力，不隐式接受其他版本。
template<class T>
const T& service(std::string_view name, uint32_t version) {
    const void* table = nullptr;
    check(HostServices::query({name.data(), name.size()}, version, &table, nullptr) == DFY_V2_OK && table, "service missing");
    const auto* header = static_cast<const dfy_v2_api_header*>(table);
    check(header->struct_size == sizeof(T) && header->interface_version == version, "service version mismatch");
    return *static_cast<const T*>(table);
}
}
// 以真实 C DLL 递归消费嵌套事件，保留叶字段仍需承担原事件及读取预算。
// 步骤：构造嵌套事件 → 协商版本 2 并读取 → 拒绝深度不足且回收 → 保留叶子后释放父链并检查预算。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 2, "expected C nested consumer");
        dfy::platform::DynamicLibrary module(std::filesystem::canonical(argv[1]));
        const auto query = reinterpret_cast<dfy_v2_query_fn>(module.symbol(DFY_V2_QUERY_EXPORT));
        const dfy_v2_plugin_api* bootstrap = nullptr;
        check(query(2, &bootstrap, nullptr) == DFY_V2_OK && bootstrap, "bootstrap failed");
        const void* table = nullptr;
        check(bootstrap->query_interface({DFY_TEST_NESTED_ID, sizeof(DFY_TEST_NESTED_ID)-1}, 1, &table, nullptr) == DFY_V2_OK, "consumer missing");
        const auto* consumer = static_cast<const dfy_test_nested_api*>(table);
        check(consumer && consumer->header.struct_size == sizeof(*consumer) && consumer->header.interface_version == 1 && consumer->inspect,
            "invalid nested consumer");
        const FieldDescriptor text{"text", ValueKind::string, 3};
        auto leaf = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.leaf", 1, {&text,1}, {1,32}));
        const NamedValue text_value{"text", Value::from_string(std::string("A\0B",3),3)};
        auto child = std::make_shared<const StructValue>(StructValue::create(leaf, {&text_value,1}, 3));
        const FieldDescriptor fields[] = {{"left", ValueKind::structure, 0, leaf}, {"right", ValueKind::structure, 0, leaf}};
        auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.outer", 1, fields, {4,128,2}));
        const NamedValue values[] = {{"left", Value::from_structure(child)}, {"right", Value::from_structure(child)}};
        auto payload = std::make_shared<const StructValue>(StructValue::create(type, values, 6));
        auto store = std::make_shared<dfy::events::EventStore>(4, 6);
        auto queue = std::make_shared<dfy::events::SubscriptionQueue>(store, 1);
        auto reader = std::make_shared<dfy::events::SubscriptionReader>(queue, 1);
        HostServices host(24); const auto receiver = host.expose(reader); auto binding = host.bind();
        const auto& refs = service<dfy_v2_reference_api>(DFY_V2_REFERENCE_INTERFACE, 1);
        const auto& reads = service<dfy_v2_reader_api>(DFY_V2_READER_INTERFACE, 1);
        const auto& structures = service<dfy_v2_struct_api_v2>(DFY_V2_STRUCT_INTERFACE, 2);
        const auto& data = service<dfy_v2_value_api_v2>(DFY_V2_VALUE_INTERFACE, 2);
        dfy_v2_handle request{}, root{}, field{}, structure{}, held{};
        check(reads.next(receiver, &request, nullptr) == DFY_V2_OK, "read submission failed");
        const auto event = store->accept(payload); queue->push(event); store->release(event); host.advance();
        check(reads.payload(request, &root, nullptr) == DFY_V2_OK, "root payload missing");
        dfy_test_nested_counts count{};
        check(consumer->inspect(HostServices::query, root, 2, &count, nullptr) == DFY_V2_OK && count.leaves == 2 && count.bytes == 6 &&
            host.references() == 3, "recursive C read failed or leaked");
        check(consumer->inspect(HostServices::query, root, 1, &count, nullptr) == DFY_V2_RESOURCE_EXHAUSTED && !count.leaves &&
            !count.bytes && host.references() == 3, "recursive failure leaked references");
        check(structures.field(root, 0, &field, nullptr) == DFY_V2_OK && data.structure(field, &structure, nullptr) == DFY_V2_OK &&
            structures.field(structure, 0, &held, nullptr) == DFY_V2_OK, "nested alias failed");
        refs.release(root, nullptr); refs.release(field, nullptr); refs.release(structure, nullptr); refs.release(request, nullptr); host.advance();
        dfy_v2_value_view view{};
        check(store->references() == 1 && reader->size() == 1 && data.read(held, &view, nullptr) == DFY_V2_OK &&
            std::string_view(view.data.string.data, view.data.string.length) == std::string_view("A\0B",3), "leaf escaped parent budget or data");
        dfy_v2_handle invalid{};
        check(data.structure(held, &invalid, nullptr) == DFY_V2_INVALID_ARGUMENT && !invalid.id, "scalar treated as structure");
        refs.release(held, nullptr); host.advance();
        check(!store->references() && !reader->size(), "leaf held finished event");
        reader->close(dfy::events::CloseMode::cancel); host.advance(); refs.release(receiver, nullptr);
        check(!host.references() && !host.resources(), "nested host leaked");
        std::cout << "Versioned C nested checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
