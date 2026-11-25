#pragma once
#include <dafeiyu/model_messages.hpp>
#include <cstddef>

namespace dfy::agent {
struct ContextWindow {
    std::uint32_t tokens = 0, output = 0;
    std::uint64_t evidence_epoch = 0;
    bool thinking = false;
};
// 在模型副本上缩短已注册回执，必要时整批归档已结算证据。原历史继续由会话持有。
std::size_t fit_context_request(model::Request& request, ContextWindow window, int head_delta = 0);
}
