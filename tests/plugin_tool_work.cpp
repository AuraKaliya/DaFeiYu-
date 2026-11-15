#include <dafeiyu/plugin_tool_work.hpp>
#include <dafeiyu/tool_executor.hpp>
#include <iostream>

namespace {
using namespace dfy::agent;
using namespace dfy::plugins;
using namespace dfy::runtime;
// 报告原生工作适配的状态或代码寿命违约。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 构造宿主已校验的固定参数描述，实际工厂经 C ABI 调用原实例。
ToolDescription description(const PluginInstance& instance) {
    return {"test.tools/increment",instance.generation(),1,{"increment","fixture",R"({"type":"object","properties":{},"additionalProperties":false})"},true,false};
}
}
// 经正式目录和驱动验证真实 C 工具结果、清理失败、取消与实例卸载。
// 步骤：创建实例并绑定工厂 → 验证结果先于退出及释放重试 → 验证协议失败后停止 → 清空代码持有并卸载。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"expected package and settings"); const auto manifest = load_manifest(argv[1]);
        dfy::platform::DynamicLibrary control(manifest.entry);
        const auto mode = reinterpret_cast<void (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_tool_test_mode"));
        const auto count = reinterpret_cast<uint32_t (DFY_V2_CALL*)(uint32_t)>(control.symbol("dfy_tool_test_count"));
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(std::make_shared<dfy::events::EventStore>(8,0),2,2);
        auto instance = std::make_shared<PluginInstance>(manifest,argv[2],channel);
        check(!instance->create() && !instance->start(),"instance start failed");
        bool invalid_name = false;
        try { (void)native_tool_factory(instance,"bad.name",1); } catch (const std::invalid_argument&) { invalid_name = true; }
        check(invalid_name,"invalid native tool identity accepted");
        ToolCatalog catalog; catalog.add(description(*instance),native_tool_factory(instance,"increment",1));
        const std::vector<std::string> ids{"test.tools/increment"}; const auto snapshot = catalog.snapshot(ids,true);
        const AttemptId attempt{StepId{RunId{1,1},1},1};
        {
            ToolExecutor executor({1,1},snapshot); auto request = executor.submit(attempt,{"one","increment","{}"}); executor.advance();
            check(request->operation()->state() == OperationState::succeeded && request->exit()->state() == OperationState::pending &&
                request->outcome().effect == ToolEffect::applied && count(1) == 1,"native result or delayed exit lost");
            check(instance->shutdown() == ShutdownResult::in_use,"active native work did not retain code");
            mode(1); executor.advance();
            check(request->operation()->state() == OperationState::succeeded && request->exit()->state() == OperationState::pending &&
                request->fault() == ToolError::release_failed && instance->shutdown() == ShutdownResult::in_use,"release failure lost responsibility or was hidden");
            executor.advance(); check(!executor.busy() && count(1) == 1 && count(2) == 1,"cleanup retry repeated native effect");
        }
        {
            ToolExecutor executor({1,1},snapshot); mode(2); auto request = executor.submit(attempt,{"two","increment","{}"}); executor.advance();
            check(request->operation()->state() == OperationState::failed && request->fault() == ToolError::read_failed &&
                request->exit()->state() == OperationState::pending,"native poll failure lost pending work");
            executor.advance();
            check(!executor.busy() && request->outcome().effect == ToolEffect::not_applied && count(1) == 1,
                "protocol failure resumed an unstarted native action");
        }
        {
            ToolExecutor executor({1,1},snapshot); mode(5); auto request = executor.submit(attempt,{"three","increment","{}"}); executor.advance();
            check(request->operation()->state() == OperationState::failed && request->fault() == ToolError::invalid_result,"malformed native result accepted");
            executor.advance(); check(!executor.busy() && request->outcome().effect == ToolEffect::applied && count(1) == 2,
                "recovered native response erased an existing effect");
        }
        {
            ToolExecutor executor({1,1},snapshot); mode(6); auto request = executor.submit(attempt,{"four","increment","{}"}); executor.advance();
            check(request->operation()->state() == OperationState::succeeded && request->fault() == ToolError::release_failed &&
                request->exit()->state() == OperationState::pending,"same-poll cleanup fault overwrote completed business result");
            executor.advance(); check(!executor.busy() && count(1) == 3,"same-poll release retry repeated action");
        }
        check(instance->shutdown() == ShutdownResult::closed,"idle factory or retained receipts pinned plugin code");
        ToolExecutor stale({1,1},snapshot); auto rejected = stale.submit(attempt,{"stale","increment","{}"}); stale.advance();
        check(rejected->operation()->state() == OperationState::failed && rejected->outcome().effect == ToolEffect::not_applied && count(0) == 4,
            "stale factory called an unloaded instance");
        std::cout << "Native tool work checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
