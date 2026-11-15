#include <dafeiyu/tool_schema.hpp>
#include <nlohmann/json.hpp>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
using dfy::agent::ToolSchema;
using Json = nlohmann::json;
// 断言非法 schema 或参数被拒绝，不回显输入正文。
void rejects(const auto& action) {
    try { action(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("invalid tool schema or arguments accepted");
}
// 创建含嵌套数组、枚举与必需字段的封闭对象契约。
Json fixture() {
    return {{"type","object"},{"properties",{{"target",{{"type","string"},{"enum",{"a","b"}}}},
        {"values",{{"type","array"},{"items",{{"type","integer"}}}}}}},
        {"required",{"target"}},{"additionalProperties",false}};
}
// 验证参数契约拒绝不支持字段，并且保留 JSON 类型和数值精度。
// 步骤：接受有界合法参数 → 拒绝缺失、额外与错误类型 → 校验契约及容量边界。
void schemas() {
    const auto source = fixture().dump(); ToolSchema schema(source); const auto copy = schema;
    copy.validate(R"({"target":"a","values":[1,18446744073709551615]})");
    for (const auto* input : {"{}",R"({"target":"c"})",R"({"target":"a","extra":1})",
        R"({"target":"a","values":[1.5]})",R"({"target":"a","values":["1"]})","null","{"})
        rejects([&] { schema.validate(input); });
    auto value = Json{{"target","a"},{"values",std::vector<int>(65,1)}};
    rejects([&] { schema.validate(value.dump()); });
    rejects([&] { schema.validate(std::string(64*1024+1,'x')); });
    auto definition = fixture(); definition["$ref"] = "https://example.invalid/schema";
    rejects([&] { ToolSchema invalid(definition.dump()); });
    definition = fixture(); definition["additionalProperties"] = true;
    rejects([&] { ToolSchema invalid(definition.dump()); });
    definition = fixture(); definition["required"] = {"unknown"};
    rejects([&] { ToolSchema invalid(definition.dump()); });
    definition = fixture(); definition["properties"]["target"]["enum"] = {"a","a"};
    rejects([&] { ToolSchema invalid(definition.dump()); });
    definition = {{"type","integer"}};
    for (int i = 0; i < 9; ++i) definition = {{"type","array"},{"items",definition}};
    rejects([&] { ToolSchema invalid(definition.dump()); });
    if (schema.json() != source) throw std::runtime_error("validation changed original schema");
}
}
// 运行纯内存参数契约检查，不调用工具或模型。
int main() {
    try { schemas(); std::cout << "Tool schema checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
