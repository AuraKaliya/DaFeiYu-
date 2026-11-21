#pragma once
#include <dafeiyu/routed_event.hpp>
#include <variant>

namespace dfy::plugins { class PluginManager; }
namespace dfy::app {
class InteractionRoutes;
class RunLog;

struct MailRoute {};
struct AskRoute {
    std::uint64_t item, revision, wait_ms;
    std::string question, recipient, channel;
    std::vector<std::string> choices;
    bool via_main;
};
struct AskFeedback {
    enum class Action { presented, answer };
    std::uint64_t ask;
    std::string channel, recipient, answer;
    Action action;
    bool delivered;
};
struct AskReceipt {
    events::EventId origin;
    std::uint64_t ask, item, revision;
    std::string disposition;
};
using EventRoute = std::variant<MailRoute,AskRoute,AskFeedback,AskReceipt>;

// 从插件宿主取出一件已解析事件，验证投影并分派业务。
void dispatch_event(plugins::PluginManager& plugins, InteractionRoutes& interactions, RunLog* log);
}
