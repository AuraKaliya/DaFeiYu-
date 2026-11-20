#include "application.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 返回已经从原插件取得的确认事项值副本。
std::optional<CompanionConfirmation> Application::companion_confirmation() const {
    return companion_ ? companion_->confirmation() : std::nullopt;
}

// 只允许本体调用者提交当前事项的原修订。
void Application::decide_companion_confirmation(std::uint64_t id, std::uint64_t revision, bool approve) {
    if (!companion_ || !session_.accepting()) throw std::invalid_argument("companion: confirmation unavailable");
    companion_->reply_confirmation(id,revision,approve);
}

// 从原信箱确认唤醒主 Agent，并且只授予当前问题的两种呈现工具。
// 步骤：检查主会话空闲及原确认未路由 → 绑定无参数渠道工具 → 接纳主回合并保持权限到退出。
bool Application::review_companion_confirmation() {
    if (conversation_.busy()) return false;
    confirmation_channels_.reset();
    if (!conversation_.accepting() || !companion_ || companion_->closed() || !companion_->confirmation()) return false;
    const auto& pending = *companion_->confirmation();
    if (pending.id == routed_confirmation_ || !pending.decision.empty() || pending.answered) return false;
    routed_confirmation_ = pending.id;
    try {
        auto catalog = std::make_shared<agent::ToolCatalog>(2);
        const std::array<std::string,2> ids{"app.confirmation/panel","app.confirmation/game"};
        constexpr auto schema = R"({"type":"object","properties":{},"additionalProperties":false})";
        const auto owner = companion_->id();
        for (std::size_t index = 0; index < ids.size(); ++index) {
            const bool game = index == 1;
            const auto name = game ? "present_confirmation_in_game" : "present_confirmation_on_panel";
            const auto text = game ? "Ask the same bound player inside Minecraft." : "Present the original question to the DaFeiYu user.";
            // 只呈现原事项；新会话或已失效的原请求会产生明确工具失败。
            const auto present = [this,owner,id = pending.id,game](const agent::ToolInvocation&) {
                if (!companion_ || companion_->id() != owner) throw std::invalid_argument("companion: original session unavailable");
                auto message = companion_->route_confirmation(id,game);
                return agent::completed_tool({true,agent::ToolEffect::applied,std::move(message),agent::ToolError::none});
            };
            catalog->add({ids[index],mailbox_host_.identity,1,{name,text,schema},true},present);
        }
        agent::RunOptions options;
        options.origin = agent::RunOrigin::mailbox;
        options.now = tool_clock_;
        options.policy.model_limit = 2;
        options.policy.tool_limit = 2;
        options.tools = catalog->snapshot(ids,true);
        options.confirmation_caller = caller_;
        options.system_instructions = "伙伴从原游戏信箱请求玩家批准。调用一个工具选择问题的投递渠道。"
            "批准或拒绝只能由玩家作出。选择本体渠道后，把工具返回的原问题和操作命令告诉用户。";
        const auto prompt = nlohmann::json{{"category","mc.confirmation.request"},{"id",pending.id},{"revision",pending.revision},
            {"question",pending.question},{"arguments",nlohmann::json::parse(pending.arguments)},{"game",pending.world}}.dump();
        conversation_.submit(prompt,std::move(options));
        confirmation_channels_ = std::move(catalog);
        return true;
    } catch (const std::exception&) {
        // 原问题仍可在受信入口读取，模型故障不改变业务裁定。
        return false;
    }
}
}
