#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/type_catalog.hpp>
#include <dafeiyu/handle_registry.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在嵌套描述、载荷或预算违反约定时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证预期拒绝类别，其他异常保留给入口定位。
template<class Error, class Action>
void rejects(Action action) {
    try { action(); } catch (const Error&) { return; }
    throw std::runtime_error("nested input unexpectedly accepted");
}
// 创建单字段叶类型，名称与版本相同但字节约束可用于冲突验证。
std::shared_ptr<const StructDescriptor> leaf(std::size_t bytes = 3) {
    const FieldDescriptor field{"text", ValueKind::string, bytes};
    return std::make_shared<const StructDescriptor>(StructDescriptor::create("test.leaf", 1, {&field, 1}, {1, 32}));
}
// 验证定义身份、展开预算和不可递归契约。
// 步骤：构造父子快照 → 触发各层预算 → 拒绝身份冲突 → 核对目录定义先后。
void descriptions() {
    const auto child = leaf();
    const FieldDescriptor field{"child", ValueKind::structure, 0, child};
    const auto parent = StructDescriptor::create("test.parent", 1, {&field,1}, {2, 30, 2});
    check(parent.depth() == 2 && parent.total_fields() == 2 && parent.name_bytes() == 29, "expanded accounting wrong");
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.parent", 1, {&field,1}, {2, 30}); });
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.parent", 1, {&field,1}, {1, 30, 2}); });
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.parent", 1, {&field,1}, {2, 28, 2}); });
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.leaf", 1, {&field,1}, {2, 64, 2}); });
    const FieldDescriptor conflict[] = {field, {"other", ValueKind::structure, 0, leaf(4)}};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.parent", 1, conflict, {4, 128, 2}); });
    FieldDescriptor malformed{"value", ValueKind::boolean, 0, child};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.parent", 1, {&malformed,1}, {2, 64, 2}); });
    malformed = {"value", ValueKind::structure, 0};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.parent", 1, {&malformed,1}, {2, 64, 2}); });
    TypeCatalog catalog(3, {8, 128, 3});
    try { (void)catalog.define(parent); check(false, "missing dependency accepted"); }
    catch (const DefinitionError& e) { check(e.reason() == DefinitionFailure::unknown_dependency && !catalog.size(), "wrong dependency failure"); }
    catalog.define(*child);
    const auto registered = catalog.define(parent);
    check(registered->equivalent(parent), "catalog changed nested definition");
    const FieldDescriptor different{"child", ValueKind::structure, 0, leaf(4)};
    const auto altered = StructDescriptor::create("test.parent", 1, {&different,1}, {2, 64, 2});
    try { (void)catalog.define(altered); check(false, "nested conflict accepted"); }
    catch (const DefinitionError& e) { check(e.reason() == DefinitionFailure::conflicting_definition, "nested conflict misclassified"); }
}
// 验证嵌套载荷按完整定义匹配，父引用保持子数据并按逻辑正文计费。
// 步骤：构造叶载荷 → 嵌入父载荷 → 检查错误定义和预算 → 释放输入后反射读取。
void values() {
    auto child_type = leaf();
    const NamedValue text{"text", Value::from_string(std::string("A\0B", 3), 3)};
    auto child = std::make_shared<const StructValue>(StructValue::create(child_type, {&text,1}, 3));
    const std::weak_ptr<const StructValue> lifetime = child;
    const FieldDescriptor field{"child", ValueKind::structure, 0, child_type};
    auto parent_type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.parent", 1, {&field,1}, {2, 64, 2}));
    auto input = std::make_unique<NamedValue>(NamedValue{"child", Value::from_structure(child)});
    auto parent = std::make_shared<const StructValue>(StructValue::create(parent_type, {input.get(),1}, 3));
    check(parent->node_count() == 3 && input->value.node_count() == 2 && input->value.payload_bytes() == 3, "nested node accounting wrong");
    rejects<std::length_error>([&] { (void)StructValue::create(parent_type, {input.get(),1}, 3, 2); });
    rejects<std::length_error>([&] { (void)StructValue::create(parent_type, {input.get(),1}, 2); });
    auto wrong = std::make_shared<const StructValue>(StructValue::create(leaf(4), {&text,1}, 3));
    const NamedValue mismatch{"child", Value::from_structure(wrong)};
    rejects<std::invalid_argument>([&] { (void)StructValue::create(parent_type, {&mismatch,1}, 3); });
    rejects<std::invalid_argument>([] { (void)Value::from_structure({}); });
    input.reset(); child.reset(); child_type.reset(); parent_type.reset();
    check(!lifetime.expired() && parent->payload_bytes() == 3 &&
        parent->at(0).as_structure().at(0).as_string() == std::string_view("A\0B",3), "nested data lost");
    parent.reset(); check(lifetime.expired(), "nested data retained after last parent");
}
// 验证深度上界和共享描述按出现次数展开计费，避免小 DAG 隐藏指数规模。
void limits() {
    auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("depth.n0", 1, {}, {0, 16}));
    for (std::size_t depth = 2; depth <= 64; ++depth) {
        const FieldDescriptor field{"child", ValueKind::structure, 0, type};
        type = std::make_shared<const StructDescriptor>(StructDescriptor::create("depth.n" + std::to_string(depth), 1, {&field,1}, {64, 2048, 64}));
    }
    const FieldDescriptor too_deep{"child", ValueKind::structure, 0, type};
    rejects<std::length_error>([&] { (void)StructDescriptor::create("depth.end", 1, {&too_deep,1}, {65, 4096, 64}); });
    rejects<std::invalid_argument>([] { (void)StructDescriptor::create("bad.depth", 1, {}, {0, 16, 65}); });
    const auto child = leaf();
    const FieldDescriptor repeated[] = {{"a", ValueKind::structure, 0, child}, {"b", ValueKind::structure, 0, child}};
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.repeat", 1, repeated, {3, 128, 2}); });
}
}
// 验收有界嵌套结构，不把旧 C 数据能力视为自动兼容新种类。
int main() {
    try { descriptions(); values(); limits(); std::cout << "Nested structure checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
