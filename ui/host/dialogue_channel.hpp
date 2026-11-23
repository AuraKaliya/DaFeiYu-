#pragma once
#include "conversation.hpp"

namespace dfy::ui {
enum class DialogueCommandState { unavailable, queued, accepted, busy, rejected, conflict, expired };

// 从业务线程接纳前的排队到正式 Session 回合，复用原窗口命令身份。
struct DialogueReceipt {
    std::uint64_t command;
    DialogueCommandState state;
    agent::RunId run;
};

// 借用已发布的不可变正文，浏览器线程可以在应用锁外编码 JSON。
struct DialogueView {
    std::shared_ptr<const app::ConversationDisplay> display;
    std::uint64_t latest_command;
    bool accepting;
    std::optional<DialogueReceipt> receipt;
};
}
