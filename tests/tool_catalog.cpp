#include <dafeiyu/tool_catalog.hpp>
#include <iostream>
#include <optional>
#include <stdexcept>

namespace {
using namespace dfy::agent;
// 报告目录与授权行为不符。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 验证非法注册、权限或参数被拒绝。
void rejects(const auto& action) {
    try { action(); } catch (const std::logic_error&) { return; }
    throw std::runtime_error("invalid tool catalog operation accepted");
}
// 生成独立实例的最小封闭参数工具描述。
ToolDescription tool(std::string id, std::string name, dfy::runtime::DomainId instance, bool changes = false) {
    return {std::move(id),instance,1,{std::move(name),"fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},changes,false};
}
// 验证目录容量、固定授权、撤销和重新注册后的身份隔离。
// 步骤：校验注册事务 → 建立权限快照 → 禁用与恢复 → 同名重载仍拒绝旧快照。
void catalog() {
    ToolCatalog catalog(2); const auto first = catalog.add(tool("fixture/read","read",1));
    rejects([&] { catalog.add(tool("fixture/read","other",2)); });
    rejects([&] { catalog.add(tool("other/read","read",2)); });
    const auto second = catalog.add(tool("fixture/write","write",1,true));
    rejects([&] { catalog.add(tool("full","full",3)); });
    const std::vector<std::string> read_ids{"fixture/read"}, write_ids{"fixture/write"};
    const auto snapshot = catalog.snapshot(read_ids);
    check(snapshot.resolve("read","{}").registration == first && snapshot.definitions().size() == 1,"wrong registered tool selected");
    rejects([&] { (void)snapshot.resolve("write","{}"); });
    rejects([&] { (void)snapshot.resolve("read",R"({"unexpected":1})"); });
    rejects([&] { (void)catalog.snapshot(write_ids); });
    check(catalog.snapshot(write_ids,true).resolve("write","{}").registration == second,"explicit action grant rejected");
    catalog.enable(first,false); rejects([&] { (void)snapshot.resolve("read","{}"); });
    catalog.enable(first,true); check(snapshot.resolve("read","{}").description.instance == 1,"reenable changed instance");
    catalog.remove(first); const auto replacement = catalog.add(tool("fixture/read","read",2));
    rejects([&] { (void)snapshot.resolve("read","{}"); });
    rejects([&] { catalog.enable(first,true); });
    check(replacement != first && catalog.snapshot(read_ids).resolve("read","{}").description.instance == 2,
        "reload reused authorization for a prior instance");
    check(catalog.snapshot({}).definitions().empty(),"empty grant exposed tools");
    const std::vector<std::string> duplicate{"fixture/read","fixture/read"};
    rejects([&] { (void)catalog.snapshot(duplicate); });
}
// 验证目录退出撤销外部快照，且失败描述不会占据注册槽。
void lifetime() {
    std::optional<ToolSnapshot> retained;
    {
        ToolCatalog catalog(1); auto invalid = tool("id","name",1); invalid.model.parameters = "{}";
        rejects([&] { catalog.add(invalid); });
        catalog.add(tool("id","name",1)); const std::vector<std::string> ids{"id"}; retained = catalog.snapshot(ids);
    }
    rejects([&] { (void)retained->resolve("name","{}"); });
    rejects([] { ToolCatalog invalid(65); });
}
}
// 运行工具描述与授权检查，本单元不执行模型或原生工具。
int main() {
    try { catalog(); lifetime(); std::cout << "Tool catalog checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
