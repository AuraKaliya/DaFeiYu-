#include "mail_reply_tool.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 把原信身份保存在工具实现中，模型只提交空参数的转发意图。
// 步骤：注册回合工具 → 固定授权范围 → 返回维持注册寿命的目录。
std::shared_ptr<agent::ToolCatalog> bind_mail_reply(runtime::DomainId host, ReplyBinding binding,
    ReplyIntent intent, agent::RunOptions& options) {
    const std::string id = "interaction/reply";
    auto catalog = std::make_shared<agent::ToolCatalog>(1);
    catalog->add({id,host,1,{"mail_reply","将本封信所载的原答复交回原提问插件。参数为空，回执区分转交意图已接纳和实际送达。",
        R"({"type":"object","properties":{},"additionalProperties":false})"},true,true},
        [binding,intent = std::move(intent)](const agent::ToolInvocation& invocation) {
            auto result = agent::completed_tool({true,agent::ToolEffect::applied,nlohmann::json{
                {"ask",binding.ask},{"status","reply_accepted"}}.dump(),agent::ToolError::none});
            intent(binding,invocation);
            return result;
        });
    options.tools = catalog->snapshot(std::span(&id,1),true).bind_arguments(id,"{}");
    options.immutable_tool_arguments = "mail_reply";
    return catalog;
}
}
