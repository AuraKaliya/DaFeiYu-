#pragma once
#include "conversation.hpp"
#include <dafeiyu/plugin_manager.hpp>
#include <functional>

namespace dfy::app {
struct MailboxDelivery {
    events::MailboxId mailbox;
    runtime::DomainId sender;
    const events::EventRecord& event;
};
// 回调可在宿主内处理原件；返回正文时交给主 Agent，空值表示处理完毕。
using MailboxReview = std::function<std::optional<std::string>(const MailboxDelivery&)>;
enum class MailboxIntakeResult { idle, handled, submitted, rejected };

// 主会话入口只从宿主绑定地址取件，支持主动审查及按类别到件触发。
class MailboxIntake {
  public:
    // 绑定本体收件身份和主会话，由该应用线程推进。
    MailboxIntake(plugins::PluginMailboxHost host, Conversation& conversation)
        : host_(std::move(host)), conversation_(conversation) {}
    // 登记已存在信箱及审查回调，空触发列表表示只主动收件。
    void watch(events::MailboxId mailbox, std::span<const events::MailboxCategory> wake, MailboxReview review);
    // 在主会话可接纳时主动处理一封，队首按本收件地址的规则审查。
    MailboxIntakeResult review(events::MailboxId mailbox);
    // 在有待处理到件信号时推进一个信箱，保持箱内 FIFO。
    MailboxIntakeResult advance();
  private:
    struct Route {
        events::MailboxId mailbox;
        runtime::DomainId sender;
        MailboxReview review;
    };
    // 交付一封已绑定地址的原件，并在审查通过时提交带来源信息的主回合。
    MailboxIntakeResult dispatch(const Route& route);

    plugins::PluginMailboxHost host_;
    Conversation& conversation_;
    std::array<std::optional<Route>,36> routes_{};
    std::size_t cursor_ = 0;
    bool working_ = false;
};
}
