#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;
class BehaviorAction : public Plugin {
public:
    static constexpr auto id = "dafeiyu.action.behavior";
    static constexpr auto version = "0.2.0";
    BehaviorAction(Host host, const Json&) : Plugin(host) { host.provide("action.behavior"); }
    void on_event(const Json& event) {
        if (event.at("type") == "reply") {
            if (!event.at("error").is_null()) host.fail(event.at("call_id"), event.at("error"));
            else if (event.at("result").is_null()) host.complete(event.at("call_id"), outcome(Json::object(), "unknown", "unknown"));
            else host.complete(event.at("call_id"), outcome(event.at("result"), "confirmed"));
            return;
        }
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method == "readiness") host.complete(call.id, {{"available", true}});
        else if (call.method == "execute") host.command(call.id, "change", "behavior.set", call.input);
        else if (call.method == "query_operation") host.command_receipt(call.id, "change");
        else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(BehaviorAction)
