#include "decision_query.hpp"
#include <nlohmann/json.hpp>

namespace dfy::agent {
// 将模型提供的快照号绑定到宿主签发的原会话和代次，查回原判定时刻的完整状态。
// 步骤：注册只读契约 → 固定原存储与权限 → 合入获准工具并保留原注册。
std::unique_ptr<ToolCatalog> grant_status_reader(std::optional<ToolSnapshot>& tools,
    std::shared_ptr<DecisionStatusStore> store, std::uint64_t session, std::uint64_t epoch,
    const std::optional<ToolSnapshot>& required_read) {
    const std::string id = "host/status_read";
    auto catalog = std::make_unique<ToolCatalog>(1);
    catalog->add({id,session,1,{"status_read",
        "按宿主尾注中的 snapshot 原号读取本会话这一个版本的完整状态，包含隐式项。过期和错代次给出具体状态。",
        R"({"type":"object","properties":{"snapshot":{"type":"string"}},"required":["snapshot"],"additionalProperties":false})"},
        false,true,{},{},UINT32_MAX,0,true},[store=std::move(store),session,epoch](const ToolInvocation& input) {
        const auto envelope = nlohmann::json::parse(input.call.arguments);
        const auto& args = envelope.contains("context") ? envelope.at("arguments") : envelope;
        const auto& ref = args.at("snapshot").get_ref<const std::string&>();
        return completed_tool({true,ToolEffect::not_applied,store->read(session,epoch,ref),ToolError::none});
    },required_read ? &*required_read : nullptr);
    const auto reader = catalog->snapshot(std::span(&id,1));
    tools = tools ? tools->include_reader(reader) : reader;
    return catalog;
}
}
