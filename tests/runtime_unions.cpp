#include <dafeiyu/union_descriptor.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_value.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在联合定义、标签或预算违反约定时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 要求当前输入按预期异常拒绝，不吞掉其他错误。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("union input accepted"); }
// 验证闭合联合标签、独立快照、声明约束与所有分支的预算。
// 步骤：构造标签边界 → 验证身份和顺序 → 拒绝无效分支 → 检查嵌套和共享描述计量。
void descriptions() {
    std::vector<UnionBranch> branches{{0,{"text",ValueKind::string,8,{},false,true}}, {UINT64_MAX,{"count",ValueKind::uint64,0}}};
    const auto type = UnionDescriptor::create("test.choice",UINT32_MAX,branches,{2,20});
    const auto same = UnionDescriptor::create("test.choice",UINT32_MAX,branches,{2,20});
    check(type.index_of(0) == 0 && type.index_of(UINT64_MAX) == 1 && !type.index_of(1) && type.tag(1) == UINT64_MAX &&
        type.depth() == 1 && type.total_fields() == 2 && type.name_bytes() == 20 && type.equivalent(same),"union tags or accounting wrong");
    branches[0].value.name = "changed";
    check(type.branches()[0].name == "text" && type.branches()[0].nullable,"union borrowed input");
    rejects<std::out_of_range>([&] { (void)type.tag(2); });
    rejects<std::invalid_argument>([] { (void)UnionDescriptor::create("test.empty",1,{}, {0,16}); });
    branches[0].value.name = "text";
    rejects<std::length_error>([&] { (void)UnionDescriptor::create("test.choice",1,branches,{1,20}); });
    rejects<std::length_error>([&] { (void)UnionDescriptor::create("test.choice",1,branches,{2,19}); });
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("test.choice",0,branches,{2,20}); });
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("invalid",1,branches,{2,20}); });
    auto invalid = branches;
    invalid[1].tag = 0;
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("test.choice",1,invalid,{2,20}); });
    invalid = branches; invalid[1].value.name = "text";
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("test.choice",1,invalid,{2,20}); });
    invalid = branches; invalid[0].value.optional = true;
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("test.choice",1,invalid,{2,20}); });
    invalid = branches; invalid[0].value.kind = ValueKind::null_value;
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("test.choice",1,invalid,{2,20}); });
    invalid = branches; invalid[0].tag = 1;
    check(!type.equivalent(UnionDescriptor::create("test.choice",UINT32_MAX,invalid,{2,20})),"union stable tag ignored");
    invalid = branches; invalid[0].value.max_bytes = 9;
    check(!type.equivalent(UnionDescriptor::create("test.choice",UINT32_MAX,invalid,{2,20})),"union payload constraint ignored");
    const UnionBranch reordered[] = {branches[1],branches[0]};
    check(!type.equivalent(UnionDescriptor::create("test.choice",UINT32_MAX,reordered,{2,20})),"union declaration order ignored");

    const EnumMember member{"only",std::int64_t{1}};
    const auto enumeration = std::make_shared<const EnumDescriptor>(EnumDescriptor::create("test.enum",1,ValueKind::int64,{&member,1},1,13));
    const UnionBranch shared[] = {{1,FieldDescriptor::enum_field("a",enumeration)}, {2,FieldDescriptor::enum_field("b",enumeration)}};
    const auto composite = UnionDescriptor::create("test.union",1,shared,{4,38});
    check(composite.total_fields() == 4 && composite.name_bytes() == 38,"union shared enum not charged per occurrence");
    rejects<std::length_error>([&] { (void)UnionDescriptor::create("test.union",1,shared,{3,38}); });
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("test.enum",1,shared,{4,64}); });
    const auto structure = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{}, {0,16}));
    const UnionBranch nested{7,{"record",ValueKind::structure,0,structure}};
    check(UnionDescriptor::create("test.union",1,{&nested,1},{1,32,2}).depth() == 2,"union nested depth lost");
    rejects<std::length_error>([&] { (void)UnionDescriptor::create("test.union",1,{&nested,1},{1,32}); });
}
// 验证选中分支的内容约束、null 语义、整数精度及聚合预算。
// 步骤：选择文本及整数分支 → 拒绝未知标签和错误内容 → 检查精确预算 → 验证共享持有与回收。
void values() {
    const UnionBranch branches[] = {{0,{"text",ValueKind::string,3,{},false,true}}, {UINT64_MAX,{"count",ValueKind::uint64,0}}};
    auto type = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.choice",1,branches,{2,20}));
    const auto text = Value::from_string(std::string("a\0b",3),3);
    const auto choice = UnionValue::create(type,0,text,11,2);
    check(choice.tag() == 0 && choice.index() == 0 && choice.value().as_string() == std::string_view("a\0b",3) &&
        choice.payload_bytes() == 11 && choice.node_count() == 2,"union selected content or accounting wrong");
    const auto empty = Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(type,0,Value::null(),8,2)));
    check(empty.kind() == ValueKind::tagged_union && empty.as_union().value().kind() == ValueKind::null_value &&
        empty.payload_bytes() == 8 && empty.node_count() == 2,"union null content collapsed into outer null");
    check(UnionValue::create(type,UINT64_MAX,Value::from_uint64(UINT64_MAX),16,2).value().as_uint64() == UINT64_MAX,"union unsigned value narrowed");
    rejects<std::invalid_argument>([&] { (void)UnionValue::create(type,1,text,11); });
    rejects<std::invalid_argument>([&] { (void)UnionValue::create(type,UINT64_MAX,Value::from_int64(1),16); });
    rejects<std::invalid_argument>([&] { (void)UnionValue::create(type,UINT64_MAX,Value::null(),8); });
    rejects<std::invalid_argument>([&] { (void)UnionValue::create({},0,text,11); });
    rejects<std::length_error>([&] { (void)UnionValue::create(type,0,Value::from_string("long",4),12); });
    rejects<std::length_error>([&] { (void)UnionValue::create(type,0,text,10); });
    rejects<std::length_error>([&] { (void)UnionValue::create(type,0,Value::null(),7); });
    for (std::size_t nodes : {0,1}) rejects<std::length_error>([&] { (void)UnionValue::create(type,0,text,11,nodes); });
    rejects<std::invalid_argument>([] { (void)Value::from_union({}); });
    rejects<std::bad_variant_access>([&] { (void)text.as_union(); });

    std::weak_ptr<const UnionValue> observed;
    {
        auto selected = std::make_shared<const UnionValue>(UnionValue::create(type,0,text,11));
        observed = selected;
        const auto held = Value::from_union(selected); selected.reset();
        check(!observed.expired() && held.as_union().tag() == 0 && held.payload_bytes() == 11,"union ownership lost");
    }
    check(observed.expired(),"union not released after last hold");
}
}
// 验收有界联合描述和不可变选择，跨 DLL 读取另行接入。
int main() {
    try { descriptions(); values(); std::cout << "Union descriptor and value checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
