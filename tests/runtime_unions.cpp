#include <dafeiyu/union_descriptor.hpp>
#include <dafeiyu/enum_descriptor.hpp>
#include <dafeiyu/union_value.hpp>
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/type_catalog.hpp>
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
// 建立单一整数分支，标签可变用于同身份冲突验收。
std::shared_ptr<const UnionDescriptor> choice_type(std::uint64_t tag = 0) {
    const UnionBranch branch{tag,{"number",ValueKind::uint64,0}};
    return std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.choice",1,{&branch,1},{1,17}));
}
// 核对类型目录拒绝的精确原因及失败后的规模。
// 步骤：记录原规模 → 执行登记 → 核对拒绝原因与原记录保持。
template<class Action>
void definition_failure(TypeCatalog& catalog, DefinitionFailure expected, Action action) {
    const auto count = catalog.size();
    try { action(); } catch (const DefinitionError& error) {
        check(error.reason() == expected && catalog.size() == count,"union definition rejection changed catalog"); return;
    }
    throw std::runtime_error("union definition unexpectedly accepted");
}
// 验证结构及联合互相组合时的内容、身份和展开约束。
// 步骤：构造联合字段并区分外层 null → 校验嵌套和共享预算 → 拒绝跨种类身份冲突 → 按依赖登记目录。
void integration() {
    const auto choice = choice_type();
    const auto field = FieldDescriptor::union_field("choice",choice,true,true);
    const auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.root",1,{&field,1},{2,32,2}));
    const auto selected = std::make_shared<const UnionValue>(UnionValue::create(choice,0,Value::from_uint64(UINT64_MAX),16,2));
    const NamedValue input{"choice",Value::from_union(selected)};
    const auto payload = StructValue::create(type,{&input,1},16,3);
    check(payload.node_count() == 3 && payload.payload_bytes() == 16 && type->depth() == 2 && type->total_fields() == 2 &&
        payload.at(0).as_union().value().as_uint64() == UINT64_MAX,"union field accounting or precision wrong");
    const NamedValue outer_null{"choice",Value::null()};
    check(!StructValue::create(type,{},0,1).present(0) && StructValue::create(type,{&outer_null,1},0,2).at(0).kind() == ValueKind::null_value,
        "outer union null or missing collapsed");
    rejects<std::length_error>([&] { (void)StructValue::create(type,{&input,1},15); });
    rejects<std::length_error>([&] { (void)StructValue::create(type,{&input,1},16,2); });
    const NamedValue mismatch{"choice",Value::from_union(std::make_shared<const UnionValue>(UnionValue::create(choice_type(1),1,Value::from_uint64(0),16)))};
    rejects<std::invalid_argument>([&] { (void)StructValue::create(type,{&mismatch,1},16); });
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.root",1,{&field,1},{2,32}); });
    const FieldDescriptor repeated[] = {field,FieldDescriptor::union_field("other",choice)};
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.root",1,repeated,{3,128,2}); });
    auto malformed = field; malformed.kind = ValueKind::uint64;
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.root",1,{&malformed,1},{2,32,2}); });
    const auto missing = FieldDescriptor::union_field("choice",{});
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.root",1,{&missing,1},{2,32,2}); });

    const UnionBranch branch{7,FieldDescriptor::union_field("nested",choice)};
    const auto outer = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.outer",1,{&branch,1},{2,64,2}));
    const auto nested = UnionValue::create(outer,7,input.value,24,3);
    check(nested.node_count() == 3 && nested.value().as_union().tag() == 0,"nested union selection lost");
    rejects<std::invalid_argument>([&] { (void)UnionDescriptor::create("test.choice",1,{&branch,1},{2,64,2}); });
    const auto structure = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.choice",1,{}, {0,17}));
    const FieldDescriptor mixed[] = {field,{"record",ValueKind::structure,0,structure}};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.root",1,mixed,{3,128,2}); });
    const FieldDescriptor reversed[] = {mixed[1],mixed[0]};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.root",1,reversed,{3,128,2}); });
    const EnumMember member{"only",std::int64_t{0}};
    const auto enumeration = EnumDescriptor::create("test.choice",1,ValueKind::int64,{&member,1},1,32);

    TypeCatalog catalog(3,{64,4096,64});
    definition_failure(catalog,DefinitionFailure::unknown_dependency,[&] { catalog.define(*outer); });
    definition_failure(catalog,DefinitionFailure::unknown_dependency,[&] { catalog.define(*type); });
    const auto registered = catalog.define(*choice);
    check(registered.get() != choice.get() && catalog.find_union("test.choice",1) == registered && !catalog.find_union("test.choice",2),"union catalog snapshot wrong");
    definition_failure(catalog,DefinitionFailure::already_defined,[&] { catalog.define(*choice); });
    definition_failure(catalog,DefinitionFailure::conflicting_definition,[&] { catalog.define(*choice_type(1)); });
    definition_failure(catalog,DefinitionFailure::conflicting_definition,[&] { catalog.define(*structure); });
    definition_failure(catalog,DefinitionFailure::conflicting_definition,[&] { catalog.define(enumeration); });
    catalog.define(*outer); catalog.define(*type);
    definition_failure(catalog,DefinitionFailure::exhausted,[&] { catalog.define(StructDescriptor::create("test.extra",1,{}, {0,16})); });
    TypeCatalog conflicting(2,{64,4096,64}); conflicting.define(*structure);
    definition_failure(conflicting,DefinitionFailure::conflicting_definition,[&] { conflicting.define(*choice); });
    TypeCatalog other_kind(2,{64,4096,64}); other_kind.define(enumeration);
    definition_failure(other_kind,DefinitionFailure::conflicting_definition,[&] { other_kind.define(*choice); });
    definition_failure(other_kind,DefinitionFailure::conflicting_definition,[&] { other_kind.define(*type); });

    auto deep = choice;
    for (unsigned depth = 2; depth <= 64; ++depth) {
        const UnionBranch next{depth,FieldDescriptor::union_field("next",deep)};
        deep = std::make_shared<const UnionDescriptor>(UnionDescriptor::create("test.level"+std::to_string(depth),1,{&next,1},{64,4096,64}));
    }
    const auto excessive = FieldDescriptor::union_field("next",deep);
    check(deep->depth() == 64,"union exact depth rejected");
    rejects<std::length_error>([&] { (void)StructDescriptor::create("test.too_deep",1,{&excessive,1},{65,4096,64}); });
}
}
// 验收有界联合描述和不可变选择，跨 DLL 读取另行接入。
int main() {
    try { descriptions(); values(); integration(); std::cout << "Union descriptor and value checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
