#include <dafeiyu/plugin_dependencies.hpp>
#include <iostream>
namespace {
using namespace dfy::plugins;
// 在依赖身份、环检查或代码保留违约时报告失败。
void check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
// 核对精确拒绝类型，不吞没无关异常。
template<class Error, class Action>
void rejects(Action action) { try { action(); } catch (const Error&) { return; } throw std::runtime_error("dependency accepted invalid action"); }
// 启动同一真实包的独立实例，依赖身份由具体实例决定而非包名。
std::shared_ptr<PluginInstance> start(const Manifest& manifest, const std::filesystem::path& data,
    const std::shared_ptr<dfy::events::BroadcastChannel>& channel) {
    auto instance = std::make_shared<PluginInstance>(manifest,data,channel);
    check(!instance->create() && !instance->start(),"dependency instance startup failed"); return instance;
}
}
// 验证真实 DLL 依赖、原子重新绑定和旧视图寿命。
// 步骤：启动独立实例 → 拒绝依赖环与超量 → 重新绑定并保持旧代码 → 逐项释放并卸载。
int wmain(int argc, wchar_t** argv) {
    try {
        check(argc == 3,"observer package and data path required"); const auto manifest = load_manifest(argv[1]);
        auto store = std::make_shared<dfy::events::EventStore>(8,0);
        auto channel = std::make_shared<dfy::events::BroadcastChannel>(store,4,1);
        auto a = start(manifest,argv[2],channel), b = start(manifest,argv[2],channel), c = start(manifest,argv[2],channel);
        PluginDependencies graph(3), foreign(1); const auto ab = graph.bind(a,b), bc = graph.bind(b,c);
        rejects<std::logic_error>([&] { (void)graph.bind(c,a); });
        rejects<std::logic_error>([&] { (void)graph.bind(a,a); });
        rejects<dfy::runtime::ReferenceError>([&] { (void)foreign.get(ab); });
        check(b->shutdown() == ShutdownResult::in_use && b->phase() == InstancePhase::active,"provider ignored active dependency");
        auto old = graph.get(ab); const auto ac = graph.rebind(ab,c);
        check(graph.size() == 3 && old->provider() == b && graph.get(ac)->provider() == c,"rebind silently replaced old provider");
        rejects<std::length_error>([&] { (void)graph.rebind(ab,c); });
        {
            auto call = old->enter();
            check(call.symbol(DFY_V2_QUERY_EXPORT) && b->shutdown() == ShutdownResult::busy,"dependency code lost activity guard");
        }
        graph.release(ab); graph.release(bc);
        check(b->shutdown() == ShutdownResult::in_use,"directory release invalidated active old view");
        old.reset(); check(b->shutdown() == ShutdownResult::closed,"old provider failed after last use");
        rejects<std::logic_error>([&] { (void)graph.rebind(ac,b); });
        check(graph.size() == 1 && graph.get(ac)->provider() == c,"failed rebind changed current binding");
        graph.release(ac); rejects<dfy::runtime::ReferenceError>([&] { graph.release(ac); });
        check(a->shutdown() == ShutdownResult::closed && c->shutdown() == ShutdownResult::closed && !channel->size() &&
            PluginInstance::retained().empty(),"dependency teardown leaked instances");
        std::cout << "Plugin dependency checks passed\n"; return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
