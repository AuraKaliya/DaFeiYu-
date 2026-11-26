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

    auto pending_write = original.prepare({{{1,1},2},1},{"pending","write","{}"});
    catalog.enable(write,false); const auto disabled = original.restrict_to(write_ids);
    pending_write->advance();
    const auto& blocked = pending_write->request()->outcome();
    check(!writes && blocked.error == ToolError::unavailable && blocked.effect == ToolEffect::not_applied &&
        blocked.output.find("before_start") != std::string::npos && blocked.output.find("disabled") != std::string::npos,"disabled work lost its last authorization check");
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
    check(delayed->request()->outcome().output.find("before_start") != std::string::npos &&
        delayed->request()->outcome().output.find("revoked") != std::string::npos,
        "revoked work lost its original registration decision");
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
// 核对宿主额外授予的读取仍然能撤销，固定单一参数的委托不能扩权。
void reader_grant() {
    const ToolFactory done = [](const auto&) { return completed_tool({true,ToolEffect::not_applied,"{}",ToolError::none}); };
    ToolCatalog tools, readers;
    tools.add(tool("fixture/write","write",2,true),done);
    const auto registration = readers.add(tool("host/read","read",3),done);
    const std::vector<std::string> action_ids{"fixture/write"}, read_ids{"host/read"};
    const auto action = tools.snapshot(action_ids,true), read = readers.snapshot(read_ids);
    const auto both = action.include_reader(read);
    check(both.resolve("read","{}").registration==registration && both.descriptions().size()==2,"reader grant lost original capability");
    rejects([&] { both.include_reader(read); });
    rejects([&] { read.include_reader(action); });
    rejects([&] { action.bind_arguments("fixture/write","{}").include_reader(read); });
    readers.enable(registration,false);
    rejects([&] { both.resolve("read","{}"); });
    check(both.resolve("write","{}").description.id=="fixture/write","reader revocation changed another capability");
}
// 插件观测授权在送模后变更时，派生状态查询的投影、解析和首次派发均沿原注册判断。
// 步骤：捕获契约并准备查询 → 禁用与恢复 → 首次执行前取消 → 撤销并同名重装。
void dependent_reader() {
    unsigned reads = 0;
    ToolCatalog plugin, host;
    const ToolFactory observed = [](const auto&) { return completed_tool({true,ToolEffect::not_applied,"observed",ToolError::none}); };
    const auto original = plugin.add(tool("mc/observe","observe",11),observed);
    const std::vector<std::string> observe_ids{"mc/observe"}, reader_ids{"host/status_read"};
    plugin.add(tool("mc/other","other",11),observed);
    const auto granted = plugin.snapshot(std::vector<std::string>{"mc/other","mc/observe"});
    const auto observation = granted.restrict_to(observe_ids);
    auto description = tool(reader_ids.front(),"status_read",12);
    description.model.parameters = R"({"type":"object","properties":{"snapshot":{"type":"string"}},"required":["snapshot"],"additionalProperties":false})";
    const ToolFactory lookup = [&](const auto&) {
        ++reads; return completed_tool({true,ToolEffect::not_applied,"frozen",ToolError::none});
    };
    const auto reader_id = host.add(description,lookup,&observation);
    const auto reader = host.snapshot(reader_ids);
    const auto both = granted.bind_context(R"({"scope":"S1"})").include_reader(reader);
    const dfy::model::ToolCall call{"query","status_read",R"({"snapshot":"1/2/status/3.1"})"};
    std::vector<ToolDescription> captured;
    const auto sent = both.ordered_definitions(&captured);
    check(sent.size() == 3 && captured.size() == 3 && both.resolve(call.name,call.arguments).registration == reader_id,"initial status grant missing");
    auto pending = both.prepare({{{1,1},1},1},call);
    plugin.enable(original,false);
    pending->advance();
    const auto next = both.ordered_definitions(&captured);
    const auto members = both.definitions();
    check(reads == 0 && next.size() == 1 && next.front().name == "other" && captured.size() == 1 &&
        captured.front().id == "mc/other" && members.size() == 1 && members.front().name == "other",
        "disabled plugin observation left status_read callable after model submission");
    const auto& blocked = pending->request()->outcome();
    check(blocked.error == ToolError::unavailable && blocked.effect == ToolEffect::not_applied &&
        blocked.output == R"({"stage":"before_start","reason":"disabled"})" && both.descriptions(false).size() == 3,
        "disabled prerequisite lost its decision or fixed membership");
    rejects([&] { both.resolve(call.name,call.arguments); });
    rejects([&] { both.prepare({{{1,1},1},2},call); });
    rejects([&] { both.restrict_to(reader_ids).bind_arguments(reader_ids.front(),call.arguments); });
    rejects([&] { observation.include_reader(reader); });
    rejects([&] { host.snapshot(reader_ids); });

    plugin.enable(original,true);
    auto cancelled = both.prepare({{{1,1},2},1},call);
    cancelled->request()->request_cancel(dfy::runtime::CancelReason::user);
    cancelled->advance();
    check(!reads && cancelled->request()->outcome().error == ToolError::cancelled,"cancelled status read started");
    auto restored = both.prepare({{{1,1},3},1},call);
    restored->advance();
    check(reads == 1 && restored->request()->outcome().output == "frozen","original grant did not restore status read");
    auto delayed = both.prepare({{{1,1},4},1},call);
    plugin.remove(original);
    const auto replacement = plugin.add(tool("mc/observe","observe",11),observed);
    host.enable(reader_id,true);
    delayed->advance();
    const auto& revoked = delayed->request()->outcome();
    const auto fresh = plugin.snapshot(observe_ids);
    check(reads == 1 && revoked.error == ToolError::unavailable && revoked.effect == ToolEffect::not_applied &&
        revoked.output == R"({"stage":"before_start","reason":"revoked"})" && reader.ordered_definitions().empty() &&
        fresh.resolve("observe","{}").registration == replacement && replacement != original,
        "same-name registration revived an old status grant");
    host.remove(reader_id);
    const auto renewed = host.add(description,lookup,&fresh);
    auto explicit_read = host.snapshot(reader_ids).prepare({{{1,1},5},1},call);
    explicit_read->advance();
    check(renewed != reader_id && reads == 2 && explicit_read->request()->outcome().succeeded,"fresh grant did not authorize new registration");
    rejects([&] { reader.resolve(call.name,call.arguments); });
}
// 只有单一、可执行、可普通读取的原注册能作为前置，失败不会消耗派生目录的注册槽。
// 步骤：构造不同授予 → 拒绝越权、失效和未实现的候选 → 注册有效项并拒绝二层依赖。
void reader_prerequisites() {
    const ToolFactory done = [](const auto&) { return completed_tool({true,ToolEffect::not_applied,"ok",ToolError::none}); };
    ToolCatalog plugin, host(1), nested(1);
    const std::vector<std::string> read_ids{"plugin/read"}, write_ids{"plugin/write"}, missing_ids{"plugin/unimplemented"};
    const auto registration = plugin.add(tool(read_ids.front(),"read",4),done);
    plugin.add(tool(write_ids.front(),"write",4,true),done);
    plugin.add(tool(missing_ids.front(),"missing",4));
    const auto original = plugin.snapshot(read_ids);
    const auto query = tool("host/query","query",9);
    const auto refuse = [&](const ToolSnapshot& grant) { rejects([&] { host.add(query,done,&grant); }); };
    refuse(plugin.snapshot({}));
    refuse(plugin.snapshot(std::vector<std::string>{read_ids.front(),missing_ids.front()}));
    refuse(plugin.snapshot(write_ids,true));
    refuse(plugin.snapshot(missing_ids));
    refuse(original.bind_arguments(read_ids.front(),"{}"));
    refuse(original.bind_context(R"({"scope":"S1"})"));
    auto change = query; change.changes_state = true;
    rejects([&] { host.add(change,done,&original); });
    rejects([&] { host.add(query,{},&original); });
    plugin.enable(registration,false); refuse(original);
    plugin.enable(registration,true);
    const auto id = host.add(query,done,&original);
    const auto derived = host.snapshot(std::span(&query.id,1));
    check(derived.resolve("query","{}").registration == id,"valid read prerequisite rejected");
    rejects([&] { nested.add(tool("host/nested","nested",10),done,&derived); });
    host.enable(id,false);
    check(derived.definitions().empty() && original.definitions().size()==1,"derived disable changed source permission");
    plugin.remove(registration);
    rejects([&] { nested.add(query,done,&original); });
}
// 原目录退出后，派生查询既不能执行，也不能替原插件继续持有实现对象。
// 步骤：交付短生命周期的前置快照 → 原目录存续时准备查询 → 目录退出后核对拒绝及资源释放。
void reader_owner_exit() {
    ToolCatalog host;
    std::weak_ptr<int> implementation;
    std::unique_ptr<ToolDriver> pending;
    const std::vector<std::string> ids{"source"}, queries{"query"};
    {
        ToolCatalog source;
        auto marker = std::make_shared<int>(1);
        implementation = marker;
        source.add(tool("source","source",1),[marker](const auto&) {
            return completed_tool({true,ToolEffect::not_applied,std::to_string(*marker),ToolError::none});
        });
        {
            const auto grant = source.snapshot(ids);
            host.add(tool("query","query",2),[](const auto&) { return completed_tool({true,ToolEffect::not_applied,"ok",ToolError::none}); },&grant);
        }
        pending = host.snapshot(queries).prepare({{{1,1},1},1},{"late","query","{}"});
    }
    check(implementation.expired(),"derived registration kept original implementation alive");
    pending->advance();
    check(pending->request()->outcome().output == R"({"stage":"before_start","reason":"revoked"})" &&
        pending->request()->outcome().effect == ToolEffect::not_applied,"expired owner allowed pending read");
    rejects([&] { host.snapshot(queries); });
}
// 运行工具描述与授权检查，本单元不执行模型或原生工具。
int main() {
    try { catalog(); lifetime(); subsets(); exact_scope(); context_scope(); reader_grant(); dependent_reader();
        reader_prerequisites(); reader_owner_exit(); std::cout << "Tool catalog checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
