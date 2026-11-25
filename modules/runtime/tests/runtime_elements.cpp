#include <dafeiyu/element_descriptor.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_descriptor.hpp>
#include <dafeiyu/map_value.hpp>
#include <iostream>
namespace {
using namespace dfy::runtime;
// 在匿名元素约束、身份或计量违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对指定拒绝类别，保留无关异常供入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("element input accepted"); }
}
// 验证匿名元素不保留名字、不允许缺失，并完整核定所有已实现子类型。
// 步骤：验收匿名标量 → 拒绝非法约束 → 验证命名子类型与展开预算 → 比较完整定义。
int main() {
    try {
        FieldDescriptor flag{"",ValueKind::boolean,0};
        const auto scalar = ElementDescriptor::create(flag,{1,0});
        check(scalar.container_depth() == 1 && scalar.total_fields() == 1 && !scalar.name_bytes() &&
            scalar.constraint().name.empty() && !scalar.constraint().optional,"scalar element accounting wrong");
        flag.nullable = true;
        const auto nullable = ElementDescriptor::create(flag,{1,0});
        check(nullable.constraint().nullable && !nullable.equivalent(scalar),"element null constraint ignored");
        flag.name = "named";
        rejects<std::invalid_argument>([&] { (void)ElementDescriptor::create(flag,{1,32}); });
        flag.name.clear(); flag.optional = true;
        rejects<std::invalid_argument>([&] { (void)ElementDescriptor::create(flag,{1,32}); });
        flag.optional = false; flag.max_bytes = 1;
        rejects<std::invalid_argument>([&] { (void)ElementDescriptor::create(flag,{1,32}); });
        rejects<std::invalid_argument>([] { (void)ElementDescriptor::create({"",ValueKind::null_value,0},{1,0}); });
        rejects<std::length_error>([] { (void)ElementDescriptor::create({"",ValueKind::boolean,0},{0,0}); });
        rejects<std::invalid_argument>([] { (void)ElementDescriptor::create(FieldDescriptor::union_field("",{}),{1,0}); });
        const auto text = ElementDescriptor::create({"",ValueKind::string,3},{1,0});
        check(!text.equivalent(ElementDescriptor::create({"",ValueKind::string,4},{1,0})),"element text limit ignored");

        const FieldDescriptor child_field{"data",ValueKind::string,4};
        const auto child = std::make_shared<const StructDescriptor>(StructDescriptor::create("runtime.element",1,{&child_field,1},{1,19}));
        const FieldDescriptor record{"",ValueKind::structure,0,child};
        const auto structured = ElementDescriptor::create(record,{2,19,2});
        check(structured.container_depth() == 2 && structured.total_fields() == 2 && structured.name_bytes() == 19,
            "anonymous container reserved a type name or lost nested accounting");
        rejects<std::length_error>([&] { (void)ElementDescriptor::create(record,{1,19,2}); });
        rejects<std::length_error>([&] { (void)ElementDescriptor::create(record,{2,18,2}); });
        rejects<std::length_error>([&] { (void)ElementDescriptor::create(record,{2,19,1}); });
        const EnumMember members[] = {{"min",INT64_MIN},{"max",INT64_MAX}};
        const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.enum",1,ValueKind::int64,members,2,15));
        const auto enum_element = ElementDescriptor::create(FieldDescriptor::enum_field("",enumeration),{3,15});
        check(enum_element.container_depth() == 1 && enum_element.total_fields() == 3,"enum element expanded incorrectly");
        const UnionBranch branch{UINT64_MAX,FieldDescriptor::enum_field("choice",enumeration)};
        const auto choice = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.union",1,{&branch,1},{3,31}));
        const auto union_element = ElementDescriptor::create(FieldDescriptor::union_field("",choice),{4,31,2});
        check(union_element.total_fields() == 4 && union_element.container_depth() == 2 && union_element.name_bytes() == 31,"union element lost metadata");
        const auto array = std::make_shared<const ArrayDescriptor>(ArrayDescriptor::create(ValueKind::uint64,2));
        const auto map = std::make_shared<const MapDescriptor>(MapDescriptor::create(ValueKind::string,2,3,4));
        check(ElementDescriptor::create(FieldDescriptor::array_field("",array),{4,64,2}).container_depth() == 2 &&
            ElementDescriptor::create(FieldDescriptor::map_field("",map),{4,64,2}).container_depth() == 2,"container element depth wrong");
        const auto copy = ElementDescriptor::create(record,{8,128,8});
        check(copy.equivalent(structured),"validation limits changed element contract");
        std::cout << "Anonymous element constraint checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
