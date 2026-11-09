#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;
class Add : public Plugin {
public:
    static constexpr auto id = "dafeiyu.action.add";
    static constexpr auto version = "0.5.0";
    Add(Host host, const Json&) : Plugin(host) { host.provide("action.add"); }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        Call call(event);
        if (call.method == "execute" || call.method == "readiness" || call.method == "query_operation") { const ActionInvocation action(call); call.input = action.arguments; if (call.method == "query_operation") call.input["id"] = action.operation_id; }
        if (call.method == "readiness") { host.complete(call.id, {{"available", true}}); return; }
        if (call.method != "execute") throw std::runtime_error("METHOD_UNSUPPORTED");
        const int a = call.input.at("a"), b = call.input.at("b");
        if (a < -1000000 || a > 1000000 || b < -1000000 || b > 1000000) throw std::runtime_error("VALUE_RANGE");
        host.complete(call.id, outcome({{"sum", a + b}}));
    }
};
DFY_PLUGIN(Add)
