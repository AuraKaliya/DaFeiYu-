#include <dafeiyu/type_catalog.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在目录状态或引用寿命违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }

// 用明确的字段形状生成测试描述，允许后续目录采用更严格的上限。
StructDescriptor describe(std::string_view name, std::uint32_t version, std::span<const FieldDescriptor> fields) {
    return StructDescriptor::create(name, version, fields, {8, 128});
}

// 验证登记失败类别，其他异常保留给测试入口定位。
// 步骤：尝试登记 → 核对拒绝类别 → 拒绝未抛错的执行结果。
void rejected(TypeCatalog& catalog, const StructDescriptor& descriptor, DefinitionFailure expected) {
    try { (void)catalog.define(descriptor); }
    catch (const DefinitionError& error) { check(error.reason() == expected, "wrong definition failure"); return; }
    throw std::runtime_error("definition unexpectedly accepted");
}

// 验证同键唯一性、精确版本、不同冲突及目录数量边界。
// 步骤：登记并查询 → 检查重复与各类冲突 → 登记另一版本 → 验证满目录和旧快照。
void verify_definitions() {
    TypeCatalog catalog(2, {2, 32});
    std::vector<FieldDescriptor> fields{{"id", ValueKind::uint64, 0}, {"text", ValueKind::string, 8}};
    const auto source = describe("app.event", 1, fields);
    const auto first = catalog.define(source);
    check(first.get() != &source && catalog.find("app.event", 1) == first && catalog.size() == 1, "definition not copied");
    for (const auto name : {"", "APP.EVENT", "app", "app.event.extra"}) {
        check(!catalog.find(name, 1), "lookup normalized or matched prefix");
    }
    check(!catalog.find("app.event", 0) && !catalog.find("app.event", 2), "lookup changed version");
    rejected(catalog, source, DefinitionFailure::already_defined);
    fields[0].name = "other";
    rejected(catalog, describe("app.event", 1, fields), DefinitionFailure::conflicting_definition);
    fields[0] = {"id", ValueKind::int64, 0};
    rejected(catalog, describe("app.event", 1, fields), DefinitionFailure::conflicting_definition);
    fields[0].kind = ValueKind::uint64;
    fields[1].max_bytes = 9;
    rejected(catalog, describe("app.event", 1, fields), DefinitionFailure::conflicting_definition);
    fields[1].max_bytes = 8;
    std::swap(fields[0], fields[1]);
    rejected(catalog, describe("app.event", 1, fields), DefinitionFailure::conflicting_definition);
    fields.pop_back();
    rejected(catalog, describe("app.event", 1, fields), DefinitionFailure::conflicting_definition);

    const auto second = catalog.define(describe("app.event", 2, fields));
    check(second != first && second->version() == 2 && catalog.size() == 2, "versions not independent");
    rejected(catalog, source, DefinitionFailure::already_defined);
    rejected(catalog, describe("app.other", 1, {}), DefinitionFailure::exhausted);
    check(catalog.find("app.event", 1) == first && first->fields()[1].max_bytes == 8 && catalog.size() == 2,
        "rejection changed catalog or snapshot");
}

// 验证目录重新施加描述预算，失败不占用名额，外部快照可跨目录退出存活。
// 步骤：拒绝非法容量 → 分别触发字段及名称预算 → 登记有效描述 → 验证最终持有者释放。
void verify_limits_and_lifetime() {
    bool invalid = false;
    try { TypeCatalog empty(0, {1, 8}); } catch (const std::invalid_argument&) { invalid = true; }
    check(invalid, "zero capacity accepted");
    std::shared_ptr<const StructDescriptor> retained;
    {
        TypeCatalog catalog(1, {1, 4});
        std::vector<FieldDescriptor> fields{{"x", ValueKind::boolean, 0}, {"y", ValueKind::boolean, 0}};
        for (const auto& descriptor : {describe("a.b", 1, fields), describe("long.name", 1, {})}) {
            bool limited = false;
            try { (void)catalog.define(descriptor); } catch (const std::length_error&) { limited = true; }
            check(limited && !catalog.size() && !catalog.find(descriptor.name(), 1), "budget rejection leaked record");
        }
        fields.pop_back();
        retained = catalog.define(describe("a.b", 1, fields));
    }
    const std::weak_ptr<const StructDescriptor> lifetime = retained;
    check(retained->name() == "a.b" && retained->fields()[0].name == "x", "catalog owned external lifetime");
    retained.reset();
    check(lifetime.expired(), "descriptor leaked after last holder");
}
}

// 验收生产类型目录的登记、拒绝与数据寿命，不装配插件权限或 C ABI。
// 步骤：验证定义唯一性 → 验证预算与寿命 → 汇报失败或成功。
int main() {
    try {
        verify_definitions();
        verify_limits_and_lifetime();
        std::cout << "Runtime type catalog checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
