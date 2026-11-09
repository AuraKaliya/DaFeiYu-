#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;
class SearchAction : public Plugin {
public:
    static constexpr auto id = "dafeiyu.action.search";
    static constexpr auto version = "0.5.0";
    SearchAction(Host host, const Json&) : Plugin(host) { host.provide("action.search"); }
    void on_event(const Json& event) {
        if (event.at("type") == "reply") {
            const auto call = event.at("call_id").get<uint64_t>();
            if (!event.at("error").is_null()) host.fail(call, event.at("error"));
            else host.complete(call, outcome(event.at("result")));
            return;
        }
        if (event.at("type") != "call") return;
        Call call(event);
        if (call.method == "execute" || call.method == "readiness" || call.method == "query_operation") { const ActionInvocation action(call); call.input = action.arguments; if (call.method == "query_operation") call.input["id"] = action.operation_id; }
        if (call.method == "readiness") { host.complete(call.id, {{"available", true}}); return; }
        if (call.method != "execute") throw std::runtime_error("METHOD_UNSUPPORTED");
        host.request(call.id, "query", "search", "query", call.input);
    }
};
DFY_PLUGIN(SearchAction)
