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
}
// 验证 UTF-8 键、确定顺序、空值区分、输入副本与完整约束匹配。
// 步骤：构造并排序 → 验证精确查询和输入隔离 → 拒绝无效及超限 → 校验父结构和定义冲突。
int main() {
    try {
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
