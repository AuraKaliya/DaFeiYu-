#include <dafeiyu/plugin.hpp>
#include <dafeiyu/contracts.hpp>
using namespace dfy::sdk;
class Reflection : public Plugin {
    std::string persona_;
public:
    static constexpr auto id = "dafeiyu.activity.reflection";
    static constexpr auto version = "0.5.0";
    Reflection(Host host, const Json& config) : Plugin(host), persona_(config.value("persona", std::string("我会保留经历的来源，并在行动后核对结果。"))) { host.provide("activity.reflection"); }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method == "evaluate_trigger") host.complete(call.id, {{"decision", "propose"}, {"text", "remember:" + persona_}});
        else if (call.method == "on_settlement") {
            validate_value(contracts::run_view_schema(), call.input.at("run"));
            for (const auto* key : {"grants", "bindings", "strategy_state", "request", "limits"}) if (call.input.at("run").contains(key)) throw std::runtime_error("ACTIVITY_PRIVATE_STATE_LEAK");
            host.complete(call.id, {{"acknowledged", true}, {"view_version", call.input.at("run").at("schema_version")}});
        }
        else throw std::runtime_error("METHOD_UNSUPPORTED");
    }
};
DFY_PLUGIN(Reflection)
