#pragma once
#include "../decision_input.hpp"
#include <algorithm>
#include <nlohmann/json.hpp>

namespace dfy::mc_test {
// 取固定模型要回答的原场景或配对工具结果，正式请求和其中的来源、调用号均保留。
// 步骤：核对末条角色、宿主来源和版本 → 分出场景消息 → 仅在普通决策中单列宿主只读工具。
inline model::Request scenario_request(model::Request request) {
    if (request.messages.size() < 2) return request;
    if (!test::is_decision_tail(request.messages.back())) return request;

    request.messages.pop_back();
    const auto origin = std::find_if(request.messages.rbegin(),request.messages.rend(),[](const model::Message& message) {
        return message.role == model::Role::user;
    });
    if (origin != request.messages.rend() && origin->source != model::MessageSource::host_plan && origin->source != model::MessageSource::host_control)
        std::erase_if(request.tools,[](const model::ToolDefinition& tool) { return tool.name == "status_read"; });
    return request;
}
}
