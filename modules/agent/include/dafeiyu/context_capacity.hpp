#pragma once
#include <dafeiyu/model_messages.hpp>
#include <cstddef>
#include <functional>
#include <span>

namespace dfy::agent {
struct ContextWindow {
    std::uint32_t tokens = 0, output = 0;
    std::uint64_t evidence_epoch = 0;
    bool thinking = false;
};
// 由宿主对连续已结算问答生成摘要，原件未取得授权时返回空。
using DialogueSummary = std::function<std::optional<std::string>(std::span<const model::Message>)>;
// 在模型副本上限制回执、引用已结算工具批次并组合完整问答，返回归档工具批次数。
std::size_t fit_context_request(model::Request& request, ContextWindow window, int head_delta = 0,
    const DialogueSummary& summarize = {});
}
