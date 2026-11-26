#pragma once
#include <dafeiyu/model_messages.hpp>
#include <nlohmann/json.hpp>
#include <span>
#include <stdexcept>

namespace dfy::tests {
// 返回固定应答的原消息视图，保留原文、事件时刻和工具调用配对。
// 步骤：核对末条角色与宿主来源 → 只略过正式 v1 决策尾注 → 返回原序列的非空业务前缀。
inline std::span<const model::Message> fixture_business_messages(std::span<const model::Message> messages) {
    if (!messages.empty()) {
        const auto& last = messages.back();
        if (last.role==model::Role::user && last.source==model::MessageSource::host_status) {
            const auto note = nlohmann::json::parse(last.content,nullptr,false);
            if (note.contains("schema") && note.at("schema")=="dafeiyu.agent_status/1")
                messages = messages.first(messages.size()-1);
        }
    }
    if (messages.empty()) throw std::runtime_error("desktop model fixture: missing business message");
    return messages;
}
}
