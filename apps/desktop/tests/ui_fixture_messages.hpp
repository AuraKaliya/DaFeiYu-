#pragma once
#include "../../../tests/decision_input.hpp"
#include <span>
#include <stdexcept>

namespace dfy::tests {
// 返回固定应答的原消息视图，保留原文、事件时刻和工具调用配对。
// 步骤：核对末条角色与宿主来源 → 只略过正式 v1 决策尾注 → 返回原序列的非空业务前缀。
inline std::span<const model::Message> fixture_business_messages(std::span<const model::Message> messages) {
    messages = test::business_messages(messages);
    if (messages.empty()) throw std::runtime_error("desktop model fixture: missing business message");
    return messages;
}
}
