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
    const ToolFactory unused = [](const auto&) -> std::unique_ptr<ToolWork> { throw std::logic_error("unexpected invocation"); };
    ToolCatalog catalog(2); const auto first = catalog.add(tool("fixture/read","read",1),unused);
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
    catalog.remove(first); const auto replacement = catalog.add(tool("fixture/read","read",2),unused);
    rejects([&] { (void)snapshot.resolve("read","{}"); });
    check(snapshot.definitions().empty(),"revoked tool remained model-visible");
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
// 验证计划步骤只缩小原范围，子快照仍共享禁用、撤销与实例隔离边界。
// 步骤：收紧原授权并执行选中工具 → 拒绝扩权和重复 → 禁用及重装后核对原身份。
void subsets() {
    using namespace dfy::runtime;
    unsigned reads = 0, writes = 0;
    ToolCatalog catalog;
    const auto read = catalog.add(tool("fixture/read","read",1),[&](const auto&) {
        ++reads; return completed_tool({true,ToolEffect::not_applied,"value",ToolError::none});
    });
    const auto write = catalog.add(tool("fixture/write","write",1,true),[&](const auto&) {
        ++writes; return completed_tool({true,ToolEffect::applied,"",ToolError::none});
    });
    const std::vector<std::string> all_ids{"fixture/read","fixture/write"}, read_ids{"fixture/read"}, write_ids{"fixture/write"};
    const auto original = catalog.snapshot(all_ids,true); const auto subset = original.restrict_to(read_ids);
    check(subset.definitions().size() == 1 && subset.resolve("read","{}").registration == read &&
        !subset.permits_state_changes() && original.permits_state_changes(),"subset changed original scope");
    auto work = subset.prepare({{{1,1},1},1},{"call","read","{}"}); work->advance();
    check(reads == 1 && !writes && work->request()->exit()->state() == OperationState::succeeded,"subset executed wrong tool");
    rejects([&] { subset.prepare({{{1,1},2},1},{"call","write","{}"}); });
    rejects([&] { subset.restrict_to(write_ids); });
    rejects([&] { original.restrict_to(std::vector<std::string>{"fixture/read","missing"}); });
    rejects([&] { original.restrict_to(std::vector<std::string>{"fixture/read","fixture/read"}); });
    check(subset.restrict_to({}).definitions().empty(),"empty subset exposed tools");

    catalog.enable(write,false); const auto disabled = original.restrict_to(write_ids);
    check(disabled.permits_state_changes() && disabled.definitions().empty(),"subset hid disabled change permission");
    rejects([&] { disabled.resolve("write","{}"); }); catalog.enable(write,true);
    check(disabled.resolve("write","{}").registration == write,"reenable rebound subset");
    auto delayed = subset.prepare({{{1,1},3},1},{"late","read","{}"});
    catalog.remove(read); catalog.add(tool("fixture/read","read",2),[&](const auto&) {
        ++reads; return completed_tool({true,ToolEffect::not_applied,"replacement",ToolError::none});
    });
    delayed->advance(); rejects([&] { original.restrict_to(read_ids).resolve("read","{}"); });
    check(reads == 1 && !writes && delayed->request()->outcome().error == ToolError::unavailable && subset.definitions().empty(),
        "revoked subset invoked replacement or old tool");
}
}
// 固定参数在重复收紧、重排字段和撤销时保持原边界。
void exact_scope() {
    using namespace dfy::agent;
    ToolCatalog catalog;
    auto description = tool("fixture/visit", "visit", 3, true);
    description.model.parameters = R"({"type":"object","properties":{"order":{"type":"integer"},"place":{"type":"string"}},"required":["order","place"],"additionalProperties":false})";
    const auto registration = catalog.add(description, [](const auto&) { return completed_tool({true, ToolEffect::applied, "ok", ToolError::none}); });
    const std::vector<std::string> ids{"fixture/visit"};
    auto base = catalog.snapshot(ids, true);
    auto bound = base.bind_arguments(ids.front(), R"({"order":2,"place":"hill"})");
    check(bound.resolve("visit", R"({ "place":"hill", "order":2 })").registration == registration, "original binding lost");
    rejects([&] { bound.restrict_to(ids).resolve("visit", R"({"order":3,"place":"hill"})"); });
    rejects([&] { bound.bind_arguments(ids.front(), R"({"order":2,"place":"other"})"); });
    rejects([&] { bound.resolve("visit", R"({"order":1,"order":2,"place":"hill"})"); });
    check(base.resolve("visit", R"({"order":3,"place":"hill"})").registration == registration, "parent scope narrowed");
    catalog.remove(registration);
    rejects([&] { bound.resolve("visit", R"({"order":2,"place":"hill"})"); });
}
// 验证工具子范围保留宿主来源，同时拒绝从模型参数替换它。
void context_scope() {
    ToolCatalog scoped;
    std::string received;
    scoped.add(tool("fixture/read","read",2),[&](const ToolInvocation& input) {
        received = input.call.arguments;
        return completed_tool({true,ToolEffect::not_applied,"ok",ToolError::none});
    });
    const std::vector<std::string> ids{"fixture/read"};
    auto source = scoped.snapshot(ids).bind_context(R"({"order":7})").restrict_to(ids);
    rejects([&] { source.bind_context(R"({"order":8})"); });
    rejects([&] { source.resolve("read",R"({"context":{"order":8}})"); });
    auto work = source.prepare({{{1,1},1},1},{"read","read","{}"});
    work->advance();
    check(received == R"({"context":{"order":7},"arguments":{}})", "native context lost");
}
// 运行工具描述与授权检查，本单元不执行模型或原生工具。
int main() {
    try { catalog(); lifetime(); subsets(); exact_scope(); context_scope(); std::cout << "Tool catalog checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
