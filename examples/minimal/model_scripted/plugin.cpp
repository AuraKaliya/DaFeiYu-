#include <dafeiyu/plugin.hpp>
using namespace dfy::sdk;

class ScriptedModel : public Plugin {
    Json config_;

public:
    static constexpr auto id = "dafeiyu.model.scripted";
    static constexpr auto version = "0.5.0";

    // 保存脚本模型配置并登记模型服务。
    ScriptedModel(Host host, Json config) : Plugin(host), config_(std::move(config)) { host.provide("model.scripted"); }

    // 为最小示例生成固定动作候选或最终回答。
    // 步骤：筛选当前意图的动作结果 → 按结果与用途选择工具调用或回答 → 回传候选及用量事件。
    void on_event(const Json& event) {
        if (event.at("type") != "call")
            return;
        const Call call(event);
        if (call.method != "generate")
            throw std::runtime_error("METHOD_UNSUPPORTED");

        Json results = Json::array();
        for (const auto& result : call.input.at("results"))
            if (result.at("intent_revision") == call.input.at("intent_revision"))
                results.push_back(result);

        Json decision;
        if (results.empty() && call.input.at("purpose") != "verify") {
            std::string action = config_.value("action", std::string("action.add"));
            Json arguments = config_.value("arguments", Json{{"a", 6}, {"b", 7}});
            decision = {{"type", "tool_call"}, {"action", action}, {"arguments", arguments}};
        } else {
            const auto result = results.empty() ? Json::object() : results.back().at("outcome");
            const auto answer = call.input.at("purpose") == "verify"
                                    ? "已复核本轮结果。"
                                    : "大肥鱼：" + result.value("data", Json::object()).dump();
            decision = {{"type", "final"}, {"text", answer},
                {"goal", result.value("status", std::string{}) == "succeeded" ? "met" : "uncertain"}};
        }

        host.complete(call.id,
            {{"events",
                Json::array({decision, {{"type", "usage"}, {"input_tokens", nullptr}, {"output_tokens", nullptr}}})}});
    }
};
DFY_PLUGIN(ScriptedModel)
