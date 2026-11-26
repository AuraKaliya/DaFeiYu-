#pragma once
#include "conversation.hpp"

namespace dfy::management {
enum class DialogueAction { send, cancel, clear, enqueue, resume, remove, append };
enum class DialogueCommandState { unavailable, queued, accepted, busy, rejected, conflict, expired, stale, full };

// 发送与排队携带原话，回合命令与待发消息命令绑定各自的原身份。
struct DialogueCommand {
    std::uint64_t id;
    DialogueAction action;
    agent::RunId target;
    std::string text;
    std::uint64_t message = 0;
    std::string client;
    std::string submission, instance;
    std::optional<std::int64_t> source_utc_ms{};
    std::int64_t stored = 0, stored_utc_ms = 0;
};

// 从业务线程接纳前的排队到正式 Session 回合，复用原窗口命令身份。
struct DialogueReceipt {
    std::uint64_t command;
    DialogueCommandState state;
    agent::RunId run;
    std::uint64_t message = 0;
    std::int64_t stored = 0;
};

// 借用已发布的不可变正文，浏览器线程可以在应用锁外编码 JSON。
struct DialogueView {
    std::shared_ptr<const app::ConversationDisplay> display;
    std::uint64_t latest_command;
    bool accepting;
    std::optional<DialogueReceipt> receipt;
};
}
