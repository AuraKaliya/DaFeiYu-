#include <dafeiyu/struct_value.hpp>
#include <dafeiyu/type_catalog.hpp>
#include <iostream>

namespace {
using namespace dfy::runtime;
// 在缺失、null 或实际内容混淆时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 要求被测试操作按指定错误类别拒绝。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("presence input accepted"); }
}
// 验证必填、可选及可空的正交约束，不补默认值或隐式转换。
// 步骤：建立约束组合 → 区分缺失与空值 → 拒绝非法输入 → 验证定义冲突。
int main() {
    try {
        auto empty = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.empty", 1, {}, {0,32}));
        const FieldDescriptor fields[] = {{"required", ValueKind::string, 0}, {"optional", ValueKind::string, 0, {}, true, false},
            {"nullable", ValueKind::string, 0, {}, false, true}, {"both", ValueKind::structure, 0, empty, true, true}};
        auto type = std::make_shared<const StructDescriptor>(StructDescriptor::create("test.presence", 1, fields, {4,128,2}));
        const NamedValue input[] = {{"required", Value::from_string({},0)}, {"nullable", Value::null()}};
        const auto value = StructValue::create(type, input, 0);
        check(value.size() == 4 && !value.payload_bytes() && value.presence(0) == FieldPresence::value &&
            value.presence(1) == FieldPresence::missing && value.presence(2) == FieldPresence::null_value &&
            value.presence(3) == FieldPresence::missing, "presence states collapsed");
        check(value.find("required") && value.find("required")->as_string().empty() && !value.find("optional") &&
            value.find("nullable") && value.find("nullable")->kind() == ValueKind::null_value, "field lookup confused null and absence");
        rejects<std::bad_optional_access>([&] { (void)value.at(1); });
        rejects<std::out_of_range>([&] { (void)value.presence(4); });
        const NamedValue missing[]{input[1]};
        rejects<std::invalid_argument>([&] { (void)StructValue::create(type, missing, 0); });
        const NamedValue invalid_null[] = {input[0], input[1], {"optional", Value::null()}};
        rejects<std::invalid_argument>([&] { (void)StructValue::create(type, invalid_null, 0); });
        const NamedValue unknown[] = {input[0], input[1], {"unknown", Value::null()}};
        rejects<std::invalid_argument>([&] { (void)StructValue::create(type, unknown, 0); });
        const NamedValue wrong_kind[] = {input[0], {"nullable", Value::from_uint64(1)}};
        rejects<std::invalid_argument>([&] { (void)StructValue::create(type, wrong_kind, 8); });
        const NamedValue null_child[] = {input[0], input[1], {"both", Value::null()}};
        check(StructValue::create(type, null_child, 0).presence(3) == FieldPresence::null_value, "nullable structure rejected null");
        auto child = std::make_shared<const StructValue>(StructValue::create(empty, {}, 0));
        const NamedValue present[] = {input[0], input[1], {"optional", Value::from_string({},0)}, {"both", Value::from_structure(child)}};
        const auto complete = StructValue::create(type, present, 0);
        check(complete.presence(1) == FieldPresence::value && complete.presence(3) == FieldPresence::value &&
            !complete.at(3).as_structure().size(), "empty content treated as null");
        const FieldDescriptor invalid_type{"null", ValueKind::null_value, 0};
        rejects<std::invalid_argument>([&] { (void)StructDescriptor::create("test.invalid", 1, {&invalid_type,1}, {1,32}); });
        TypeCatalog catalog(3, {8,128,2}); catalog.define(*empty); catalog.define(*type);
        for (bool change_optional : {false, true}) {
            std::vector<FieldDescriptor> changed(type->fields().begin(), type->fields().end());
            if (change_optional) changed[1].optional = false; else changed[2].nullable = false;
            try { catalog.define(StructDescriptor::create("test.presence", 1, changed, {4,128,2})); check(false, "constraint change accepted"); }
            catch (const DefinitionError& error) { check(error.reason() == DefinitionFailure::conflicting_definition, "wrong constraint conflict"); }
        }
        std::cout << "Field presence checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
