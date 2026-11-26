#pragma once
#include <dafeiyu/model_messages.hpp>
#include <nlohmann/json.hpp>
#include <span>

namespace dfy::test {
// 从原消息核对宿主身份和顶层协议，普通用户同文或嵌套字段保持原义。
inline bool is_decision_tail(const model::Message& last) {
    if (last.role!=model::Role::user || last.source!=model::MessageSource::host_status) return false;
    const auto note = nlohmann::json::parse(last.content,nullptr,false);
    return note.contains("schema") && note.at("schema")=="dafeiyu.agent_status/1";
}
// 取固定模型要应答的业务序列，兼容旧请求并保留真实工具与回执的位置。
inline std::span<const model::Message> business_messages(std::span<const model::Message> messages) {
    return !messages.empty() && is_decision_tail(messages.back()) ? messages.first(messages.size()-1) : messages;
}
// 结构化后端和纯文本后端共用相同的原消息选择规则。
inline std::span<const model::Message> business_messages(const model::Request& input) {
    return business_messages(std::span<const model::Message>(input.messages));
}
}
