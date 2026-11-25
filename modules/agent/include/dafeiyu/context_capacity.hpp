#pragma once
#include <dafeiyu/model_messages.hpp>
#include <cstddef>

namespace dfy::agent {
struct ContextWindow {
    std::uint32_t tokens = 0, output = 0;
    std::uint64_t evidence_epoch = 0;
    bool thinking = false;
};
// 在模型副本上限制回执、引用已结算工具批次并组合完整问答，返回归档工具批次数。
std::size_t fit_context_request(model::Request& request, ContextWindow window, int head_delta = 0);
}
