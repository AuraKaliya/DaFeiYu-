#pragma once
#include "conversation.hpp"

namespace dfy::ui {
enum class DialogueAction { send, cancel, clear };
enum class DialogueCommandState { unavailable, queued, accepted, busy, rejected, conflict, expired, stale };

// 发送携带原话，停止与清空绑定用户操作时看见的最后一轮。
struct DialogueCommand {
    std::uint64_t id;
    DialogueAction action;
    agent::RunId target;
    std::string text;
};

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
