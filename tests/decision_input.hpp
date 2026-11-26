#pragma once
#include <dafeiyu/model_messages.hpp>
#include <span>

namespace dfy::test {
// 取固定模型要应答的业务序列，兼容旧请求并保留真实工具与回执的位置。
inline std::span<const model::Message> business_messages(const model::Request& input) {
    auto messages = std::span<const model::Message>(input.messages);
    if (!messages.empty() && messages.back().source==model::MessageSource::host_status &&
        messages.back().role==model::Role::user &&
        messages.back().content.find(R"("schema":"dafeiyu.agent_status/1")")!=std::string::npos)
        return messages.first(messages.size()-1);
    return messages;
}
}
