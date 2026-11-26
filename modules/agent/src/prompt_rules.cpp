#include <dafeiyu/prompt_rules.hpp>

namespace dfy::agent {
// 同一场景及用途按固定顺序装配职责、授权、结算、投递与人格，原生规划约定进入本步输出段。
// 步骤：标出入口与事实来源 → 加入宿主执行约束 → 按实际渠道和本步用途说明输出 → 追加风格。
std::string render_prompt_rules(const PromptRules& rules, std::string_view scene, std::string_view protocol, ModelPurpose purpose) {
    const bool plugin = rules.route==OutputRoute::plugin_player || rules.route==OutputRoute::plugin_internal;
    std::string text = "【职责与信息来源 v1】\n";
    text += plugin ? "在本插件的场景与本次绑定内处理任务。" : "在本体主会话内处理当前事项。";
    text += "以来源标记区分真人原话、宿主记录和领域数据，保留原目标、修订与未决事项。";
    if (!scene.empty()) { text += '\n'; text += scene; }
    if (purpose!=ModelPurpose::plan_generation && !protocol.empty()) { text += '\n'; text += protocol; }
    text += "\n【授权与行动边界 v1】\n只请求本步提供的工具。宿主在执行前复核参数、批准、范围与现场条件。";
    text += "\n【判断与结算 v1】\n按已核实的事实继续、补查或等待。区分接纳、实效与实际退出。实效未知时沿原工作号查回执。";
    text += "\n【输出与投递 v1】\n";
    if (purpose==ModelPurpose::plan_generation) {
        text += "将计划提议交给宿主审查。";
        if (!protocol.empty()) { text += '\n'; text += protocol; }
    } else if (purpose==ModelPurpose::plan_step) text += "本步结果交给宿主核验，依据实际工作及回执结算。";
    else switch (rules.route) {
    case OutputRoute::main_chat: text += "普通最终文本交给主聊天用户。"; break;
    case OutputRoute::main_chat_with_reply: text += "普通最终文本交给主聊天用户。需要提前发言或追问时可调用 main_reply。"; break;
    case OutputRoute::plugin_player: text += "最终结果交回插件。玩家发言遵循本场景的渠道约定，以投递回执确认送达。"; break;
    case OutputRoute::main_internal: text += "结果交回本体用于内部结算。"; break;
    case OutputRoute::plugin_internal: text += "结果交回发起任务的插件用于内部结算。"; break;
    }
    text += "\n【人格与风格 v1】\n";
    text += rules.style.empty() ? "简洁表述本步结果。" : rules.style;
    return text;
}
}
