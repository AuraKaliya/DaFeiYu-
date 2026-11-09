#include <dafeiyu/plugin.hpp>
#include <dafeiyu/run_view.hpp>
using namespace dfy::sdk;
class Strategy : public Plugin {
    bool verify_;
public:
    static constexpr auto id = "dafeiyu.strategy.default";
    static constexpr auto version = "0.5.0";
    Strategy(Host host, const Json& config) : Plugin(host), verify_(config.value("verify", false)) { host.provide("strategy.default"); }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event); auto state = call.input.at("state");
        if (call.input.value("trigger", std::string{}) == "input_revised") state.erase("reviewed");
        if (call.method == "review_finish") {
            if (verify_ && !state.value("reviewed", false)) {
                state["reviewed"] = true;
                host.complete(call.id, {{"proposal", {{"type", "request_model"}, {"purpose", "verify"}}}, {"state", state}});
            } else host.complete(call.id, {{"accept", true}, {"state", state}});
            return;
        }
        if (call.method != "propose") throw std::runtime_error("METHOD_UNSUPPORTED");
        const auto view = RunView::from_json(call.input.at("run"));
        const auto& candidate = view.candidate;
        Json proposal{{"type", "request_model"}, {"purpose", "respond"}};
        if (view.observation.value("kind", std::string{}) == "model_failed") proposal = {{"type", "wait"}, {"condition", {{"type", "user"}}}};
        else if (!candidate.is_null()) {
            if (candidate.at("type") == "tool_call") proposal = {{"type", "invoke_action"}, {"action", candidate.at("action")}, {"arguments", candidate.at("arguments")}};
            else proposal = {{"type", "finish"}, {"text", candidate.at("text")}, {"goal", candidate.value("goal", std::string("uncertain"))}};
        }
        host.complete(call.id, {{"proposal", proposal}, {"state", state}});
    }
};
DFY_PLUGIN(Strategy)
