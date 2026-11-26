#pragma once
#include <string>
#include <string_view>

namespace dfy::agent {
enum class ModelPurpose { direct, plan_generation, plan_step };
enum class OutputRoute { main_chat, main_chat_with_reply, plugin_player, main_internal, plugin_internal };
struct PromptRules {
    OutputRoute route;
    std::string style;
};
// 依本次决策的职责和真实投递入口渲染有序规则，各来源段的版本随内容维护。
std::string render_prompt_rules(const PromptRules& rules, std::string_view scene,
    std::string_view protocol = {}, ModelPurpose purpose = ModelPurpose::direct);
}
