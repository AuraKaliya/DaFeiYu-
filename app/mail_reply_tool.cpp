#include "mail_reply_tool.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 把原信身份保存在工具实现中，模型只提交空参数的转发意图。
// 步骤：注册回合工具 → 固定授权范围 → 返回维持注册寿命的目录。
std::shared_ptr<agent::ToolCatalog> bind_mail_reply(runtime::DomainId host, ReplyBinding binding,
    ReplyIntent intent, agent::RunOptions& options) {
    const std::string id = "interaction/reply";
    auto catalog = std::make_shared<agent::ToolCatalog>(1);
    catalog->add({id,host,1,{"mail_reply","Return the original ASK result to the plugin that asked the question.",
        R"({"type":"object","properties":{},"additionalProperties":false})"},true,true},
        [binding,intent = std::move(intent)](const agent::ToolInvocation& invocation) {
            auto result = agent::completed_tool({true,agent::ToolEffect::applied,nlohmann::json{
                {"ask",binding.ask},{"status","reply_accepted"}}.dump(),agent::ToolError::none});
            intent(binding,invocation);
            return result;
        });
    options.tools = catalog->snapshot(std::span(&id,1),true).bind_arguments(id,"{}");
    options.system_instructions += " 本轮收到原问题的答复，调用 mail_reply 把原结果送回提问者。";
    return catalog;
}
}
