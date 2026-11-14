#include <dafeiyu/map_value.hpp>
#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/type_catalog.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在映射唯一性、顺序或预算违反契约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证明确错误类别，其他异常继续向入口报告。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("map input accepted"); }
// 建立独立映射约束。
std::shared_ptr<const MapDescriptor> schema(ValueKind kind, std::size_t count, std::size_t keys,
    std::size_t values = 0, bool nullable = false) {
    return std::make_shared<const MapDescriptor>(MapDescriptor::create(kind,count,keys,values,nullable));
}
// 验证完整映射值约束、精确键、共享数据计量和穿过映射的类型依赖。
// 步骤：构造可空结构映射 → 校验完整内容及嵌套 → 检查身份和目录 → 拒绝深度与逻辑节点超限。
void composites() {
    const FieldDescriptor text{"text",ValueKind::string,3};
    const auto record = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&text,1},{1,32}));
    const NamedValue input{"text",Value::from_string("abc",3)};
    const auto value = std::make_shared<const StructValue>(StructValue::create(record,{&input,1},3));
    const auto constraint = ElementDescriptor::create({"",ValueKind::structure,0,record,false,true},{2,32,2});
    const auto type = std::make_shared<const MapDescriptor>(MapDescriptor::create(constraint,3,3));
    const MapEntry entries[] = {{"b",Value::from_structure(value)},{std::string("a\0b",3),Value::null()},{"",Value::from_structure(value)}};
    const auto map = std::make_shared<const MapValue>(MapValue::create(type,entries,10,9));
    check(map->node_count() == 9 && map->payload_bytes() == 10 && map->at(0).key.empty() && type->depth() == 2 && type->total_fields() == 2 &&
        map->find("b")->as_structure().at(0).as_string() == "abc" && map->find(std::string_view("a\0b",3))->kind() == ValueKind::null_value && !map->find("a"),
        "composite map accounting or exact keys wrong");
    rejects<std::length_error>([&] { (void)MapValue::create(type,entries,9); });
    rejects<std::length_error>([&] { (void)MapValue::create(type,entries,10,8); });
    const FieldDescriptor changed{"text",ValueKind::string,4};
    const auto other = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.record",1,{&changed,1},{1,32}));
    const MapEntry mismatch{"x",Value::from_structure(std::make_shared<const StructValue>(StructValue::create(other,{&input,1},3)))};
    rejects<std::invalid_argument>([&] { (void)MapValue::create(type,{&mismatch,1},4); });
    check(!type->equivalent(MapDescriptor::create(ElementDescriptor::create({"",ValueKind::structure,0,other,false,true},{2,32,2}),3,3)),
        "map ignored full value definition");
    const auto outer = std::make_shared<const MapDescriptor>(MapDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::map_field("",type),{3,32,3}),1,1));
    const MapEntry nested{"x",Value::from_map(map)};
    check(MapValue::create(outer,{&nested,1},11,11).node_count() == 11,"nested map omitted key or child nodes");
    rejects<std::length_error>([&] { (void)MapValue::create(outer,{&nested,1},11,10); });
    const auto field = FieldDescriptor::map_field("items",type);
    const auto parent = StructDescriptor::create("test.parent",1,{&field,1},{3,64,3});
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.record",1,{&field,1},{3,64,3}); });
    const FieldDescriptor conflict[] = {field,{"other",ValueKind::structure,0,other}};
    rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.parent",1,conflict,{5,128,3}); });
    TypeCatalog catalog(2,{4,128,3});
    try { catalog.define(parent); check(false,"map dependency bypassed catalog"); }
    catch (const DefinitionError& error) { check(error.reason() == DefinitionFailure::unknown_dependency && !catalog.size(),"wrong map dependency rejection"); }
    catalog.define(*record); catalog.define(parent);

    auto deep = schema(ValueKind::boolean,0,0);
    for (unsigned depth = 2; depth <= 64; ++depth)
        deep = std::make_shared<const MapDescriptor>(MapDescriptor::create(ElementDescriptor::create(FieldDescriptor::map_field("",deep),{64,0,64}),0,0));
    check(deep->depth() == 64 && deep->total_fields() == 64,"anonymous map layers escaped descriptor budget");
    rejects<std::length_error>([&] { (void)ElementDescriptor::create(FieldDescriptor::map_field("",deep),{65,0,64}); });

    const auto large_type = schema(ValueKind::boolean,65536,5,0,true);
    std::vector<MapEntry> empty_entries;
    for (unsigned i = 0; i < 65536; ++i) empty_entries.push_back({std::to_string(i),Value::null()});
    const auto large = std::make_shared<const MapValue>(MapValue::create(large_type,empty_entries,400000));
    const auto repeated_type = std::make_shared<const MapDescriptor>(MapDescriptor::create(
        ElementDescriptor::create(FieldDescriptor::map_field("",large_type),{2,0,2}),8,1));
    std::vector<MapEntry> repeated;
    for (unsigned i = 0; i < 8; ++i) repeated.push_back({std::to_string(i),Value::from_map(large)});
    check(MapValue::create(repeated_type,std::span<const MapEntry>(repeated).first(7),SIZE_MAX).node_count() == 917519,"shared map node accounting wrong");
    rejects<std::length_error>([&] { (void)MapValue::create(repeated_type,repeated,SIZE_MAX,SIZE_MAX); });
}
}
// 验证 UTF-8 键、确定顺序、空值区分、输入副本与完整约束匹配。
// 步骤：构造并排序 → 验证精确查询和输入隔离 → 拒绝无效及超限 → 校验父结构和定义冲突。
int main() {
    try {
        composites();
        auto type = schema(ValueKind::uint64,4,3);
        std::vector<MapEntry> input{{"b",Value::from_uint64(2)}, {"a",Value::from_uint64(1)},
            {std::string("a\0b",3),Value::from_uint64(UINT64_MAX)}, {"",Value::from_uint64(0)}};
        auto map = std::make_shared<const MapValue>(MapValue::create(type,input,37,9));
        check(map->size() == 4 && map->payload_bytes() == 37 && map->node_count() == 9 && map->at(0).key.empty() &&
            map->at(1).key == "a" && map->at(2).key == std::string("a\0b",3) && map->at(3).key == "b", "map order or accounting wrong");
        check(map->find(std::string_view("a\0b",3))->as_uint64() == UINT64_MAX && !map->find("A") && !map->find("missing"), "map key normalized or truncated");

        rejects<std::length_error>([&] { (void)MapValue::create(type,input,36); });
        rejects<std::length_error>([&] { (void)MapValue::create(type,input,37,8); });
        rejects<std::out_of_range>([&] { (void)map->at(4); });
        input[0].key = "changed"; input.clear();
        check(map->find("b")->as_uint64() == 2 && !map->find("changed"), "map borrowed input keys");

        const MapEntry duplicate[] = {{"a",Value::from_uint64(1)}, {"a",Value::from_uint64(2)}};
        rejects<std::invalid_argument>([&] { (void)MapValue::create(type,duplicate,64); });
        const MapEntry bad_key{std::string("\xc0\x80",2),Value::from_uint64(1)};
        rejects<std::invalid_argument>([&] { (void)MapValue::create(type,{&bad_key,1},16); });
        const MapEntry long_key{"long",Value::from_uint64(1)};
        rejects<std::length_error>([&] { (void)MapValue::create(type,{&long_key,1},16); });
        const MapEntry wrong{"a",Value::from_int64(1)};
        rejects<std::invalid_argument>([&] { (void)MapValue::create(type,{&wrong,1},16); });

        const MapEntry nullable[] = {{"nil",Value::null()}, {"",Value::from_string({},0)}};
        const auto present = MapValue::create(schema(ValueKind::string,2,3,0,true),nullable,3,5);
        check(present.find("nil") && present.find("nil")->kind() == ValueKind::null_value && present.find("")->kind() == ValueKind::string &&
            !present.find("unknown"), "missing, null and empty value collapsed");
        rejects<std::invalid_argument>([&] { (void)MapValue::create(schema(ValueKind::string,2,3),nullable,3); });
        check(MapValue::create(schema(ValueKind::boolean,0,0),{},0,1).node_count() == 1, "empty map lost root");
        rejects<std::length_error>([&] { (void)MapValue::create(schema(ValueKind::boolean,0,0),{},0,0); });
        rejects<std::invalid_argument>([] { (void)MapDescriptor::create(ValueKind::array,1,8); });
        rejects<std::length_error>([] { (void)MapDescriptor::create(ValueKind::boolean,65537,0); });

        const MapEntry unicode[] = {{"\xc3\xa9",Value::from_bool(true)}, {"e\xcc\x81",Value::from_bool(false)}};
        const auto distinct = MapValue::create(schema(ValueKind::boolean,2,3),unicode,7);
        check(distinct.size() == 2 && distinct.find("\xc3\xa9")->as_bool() && !distinct.find("e\xcc\x81")->as_bool(), "keys implicitly normalized");

        const auto field = FieldDescriptor::map_field("items",type);
        auto parent = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.map",1,{&field,1},{2,64,2}));
        const NamedValue value{"items",Value::from_map(map)};
        check(StructValue::create(parent,{&value,1},37).node_count() == 10,"parent omitted map nodes");
        const auto different = FieldDescriptor::map_field("items",schema(ValueKind::uint64,4,4));
        auto other = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.map",1,{&different,1},{2,64,2}));
        rejects<std::invalid_argument>([&] { (void)StructValue::create(other,{&value,1},37); });
        TypeCatalog catalog(2,{2,128,2}); catalog.define(*parent);
        try { catalog.define(*other); check(false,"map constraint conflict accepted"); }
        catch (const DefinitionError& error) { check(error.reason() == DefinitionFailure::conflicting_definition,"wrong map conflict"); }

        const std::weak_ptr<const MapValue> lifetime = map;
        auto owned = std::make_shared<const Value>(Value::from_map(map)); map.reset();
        check(owned->as_map().find("a")->as_uint64() == 1 && !lifetime.expired(),"map lifetime lost");
        std::weak_ptr<const MapValue> released;
        {
            auto sole = std::make_shared<const MapValue>(MapValue::create(schema(ValueKind::boolean,0,0),{},0));
            released = sole;
            const auto held = Value::from_map(sole); sole.reset();
            check(!released.expired() && held.as_map().size() == 0,"map hold did not survive source");
        }
        check(released.expired(),"map not reclaimed after final hold");
        rejects<std::invalid_argument>([] { (void)Value::from_map({}); });
        std::cout << "Scalar map checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
