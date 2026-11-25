#include "application.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 仅在明确入口授予主 Agent 当前游戏信箱的收发工具。
// 步骤：冻结原地址及成员 → 注册受限的投递和回执工具 → 通过主会话接纳本轮。
agent::TurnDiagnostics Application::submit_companion_exchange(const std::string& prompt) {
    const auto address = companion_address();
    if (!address || !companion_->accepts_partner_messages(*address)) throw std::invalid_argument("companion: pair with the current player first");
    auto catalog = std::make_shared<agent::ToolCatalog>(3);
    const std::array<std::string,3> ids{"app.companion/delegate","app.companion/query","app.companion/receipts"};
    constexpr auto schema = R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"],"additionalProperties":false})";
    for (std::size_t index = 0; index < 2; ++index) {
        const bool delegate = index == 0;
        const auto definition = delegate
            ? model::ToolDefinition{"delegate_game_task","Send a task to the current game partner. This queues a request and grants no game approval.",schema}
            : model::ToolDefinition{"query_game_progress","Ask the partner for the current task's actual progress. Read receipts separately.",schema};
        // 同一轮只接受一次同类发送，精确重试返回原业务编号。
        // 步骤：核对原配对 → 解析正文 → 登记原消息或复用已有回执。
        const auto send = [this,address = *address,delegate,posted = std::optional<std::pair<std::string,std::uint64_t>>{}]
            (const agent::ToolInvocation& invocation) mutable {
            if (!companion_ || !companion_->accepts_partner_messages(address)) throw std::invalid_argument("companion: original binding unavailable");
            auto text = nlohmann::json::parse(invocation.call.arguments).at("text").get<std::string>();
            if (posted && posted->first != text) throw std::logic_error("companion: one request of each kind per turn");
            if (!posted) {
                const auto kind = delegate ? CompanionMessageKind::delegation : CompanionMessageKind::inquiry;
                const auto id = companion_->send_partner(address,kind,text);
                posted.emplace(std::move(text),id);
            }
            return agent::completed_tool({true,agent::ToolEffect::applied,nlohmann::json{{"queued",true},{"id",posted->second}}.dump(),agent::ToolError::none});
        };
        catalog->add({ids[index],address->instance,1,definition,true},send);
    }
    // 只复制已收到的原回执，调用本身不触发游戏动作。
    const auto receipts = [this,address = *address](const agent::ToolInvocation&) {
        if (!companion_) throw std::logic_error("companion: original session unavailable");
        return agent::completed_tool({true,agent::ToolEffect::not_applied,companion_->partner_receipts(address),agent::ToolError::none});
    };
    catalog->add({ids[2],address->instance,1,{"read_game_receipts","Read current verified partner replies and tool receipts.",
        R"({"type":"object","properties":{},"additionalProperties":false})"},false},receipts);
    agent::RunOptions options;
    options.now = tool_clock_;
    options.tools = catalog->snapshot(ids,true);
    options.confirmation_caller = caller_;
    options.policy.model_limit = 5;
    options.policy.tool_limit = 4;
    options.system_instructions = "按用户原意向当前游戏伙伴委托或询问。投递只代表进入队列，游戏完成以回执为准。";
    const auto run = conversation_.submit(prompt,std::move(options));
    companion_exchange_tools_ = std::move(catalog);
    return run;
}
}
