#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;
class ScriptedModel {
    Json config_;
public:
    static constexpr auto id = "dafeiyu.model.scripted";
    static constexpr auto version = "0.1.0";
    ScriptedModel(Host host, Json config) : config_(std::move(config)) {
        if (!config_.at("action").is_string() || !config_.at("arguments").is_object()) throw std::runtime_error("SCRIPT_CONFIG_INVALID");
        host.provide("model.scripted");
    }
    void start() {}
    void stop() {}
    void invoke(const Call& call, const Output& output) {
        if (call.method != "generate") throw std::runtime_error("METHOD_UNSUPPORTED");
        call.checkpoint();
        if (call.input.at("results").empty()) {
            output.emit({{"type", "tool_call"}, {"action", config_.at("action")}, {"arguments", config_.at("arguments")}});
        } else {
            const auto& outcome = call.input.at("results").back().at("outcome");
            const bool okay = outcome.at("status") == "succeeded";
            std::string text = okay ? "大肥鱼：示例动作的计算结果是 " + outcome.at("data").at("sum").dump() + "。" : "大肥鱼：动作未成功，保留失败记录。";
            text += " 本轮使用记忆版本 " + call.input.at("memory").at("revision").dump() + "。";
            output.emit({{"type", "text_delta"}, {"text", text}});
            output.emit({{"type", "final"}, {"text", text}, {"goal", okay ? "met" : "unmet"}});
        }
        output.emit({{"type", "usage"}, {"source", "scripted"}, {"input_tokens", nullptr}, {"output_tokens", nullptr}});
    }
};
DFY_PLUGIN(ScriptedModel)
