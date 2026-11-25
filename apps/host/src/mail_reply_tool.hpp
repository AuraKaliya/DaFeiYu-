#pragma once
#include <dafeiyu/run_options.hpp>
#include <dafeiyu/event_store.hpp>

namespace dfy::app {
struct ReplyBinding {
    events::EventId origin, source_message;
    runtime::DomainId owner;
    std::uint64_t ask, item, revision;
};
using ReplyIntent = std::function<void(const ReplyBinding&,const agent::ToolInvocation&)>;

// 向当前可信信件回合独占授予原答复的转发工具。
std::shared_ptr<agent::ToolCatalog> bind_mail_reply(runtime::DomainId host, ReplyBinding binding,
    ReplyIntent intent, agent::RunOptions& options);
}
