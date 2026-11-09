#include <dafeiyu/plugin.hpp>
#include <thread>
using namespace dfy::sdk;
class ScriptedModel : public Plugin {
    Json config_;
public:
    static constexpr auto id = "dafeiyu.model.scripted";
    static constexpr auto version = "0.2.0";
    ScriptedModel(Host host, Json config) : Plugin(host), config_(std::move(config)) { host.provide("model.scripted"); }
    void on_event(const Json& event) {
        if (event.at("type") != "call") return;
        const Call call(event);
        if (call.method != "generate") throw std::runtime_error("METHOD_UNSUPPORTED");
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(config_.value("delay_ms", 0));
        while (std::chrono::steady_clock::now() < until) {
            if (host.cancelled(call.id)) { host.fail(call.id, error("CANCELLED", "cancelled")); return; }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        const auto text = call.input.at("input").at("text").get<std::string>();
        Json results = Json::array();
        for (const auto& result : call.input.at("results")) if (result.at("intent_revision") == call.input.at("intent_revision")) results.push_back(result);
        Json decision;
        if (results.empty() && call.input.at("purpose") != "verify") {
            std::string action = config_.value("action", std::string("action.add"));
            Json arguments = config_.value("arguments", Json{{"a", 6}, {"b", 7}});
            if (text.starts_with("search:")) { action = "action.search"; arguments = {{"query", text.substr(7)}, {"options", {{"limit", 2}}}}; }
            if (text.starts_with("background:")) { action = "action.background"; arguments = {{"value", 21}}; }
            if (text.starts_with("remember:")) { action = "action.remember"; arguments = {{"persona", text.substr(9)}}; }
            if (text.starts_with("unknown:")) { action = "action.write_demo"; arguments = {{"value", text.substr(8)}}; }
            decision = {{"type", "tool_call"}, {"action", action}, {"arguments", arguments}};
        } else {
            const auto result = results.empty() ? Json::object() : results.back().at("outcome");
            const auto answer = call.input.at("purpose") == "verify" ? "已复核本轮结果。" : "大肥鱼：" + result.value("data", Json::object()).dump();
            decision = {{"type", "final"}, {"text", answer}, {"goal", result.value("status", std::string{}) == "succeeded" ? "met" : "uncertain"}};
        }
        host.complete(call.id, {{"events", Json::array({decision, {{"type", "usage"}, {"input_tokens", nullptr}, {"output_tokens", nullptr}}})}});
    }
};
DFY_PLUGIN(ScriptedModel)
