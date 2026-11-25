#pragma once
#include "event_dispatch.hpp"
#include <dafeiyu/mailboxes.hpp>

namespace dfy::app {
class RunLog;

// 持有一次终态投递和原信箱回执，由原事项的寿命限制容量。
class ReplyDelivery {
  public:
    // 固定原实例、信箱及结果正文。
    ReplyDelivery(runtime::DomainId owner, AskReceipt original, events::MailboxId target, std::string category, std::string body)
        : owner_(owner), original_(std::move(original)), target_(target), category_(std::move(category)), body_(std::move(body)) {}
    // 推进一次入箱，容量暂满时保留原结果，入箱后复用原回执。
    bool advance(events::Mailboxes& network, runtime::DomainId sender, RunLog* log);
    // 核对由原插件给出的实际收件凭据。
    bool acknowledge(runtime::DomainId owner, const AskReceipt& receipt, RunLog* log);
    // 读取插件是否已确认该原件。
    bool acknowledged() const noexcept { return acknowledged_; }
    // 返回实际入箱的地址和消息序号。
    events::EventId message() const noexcept { return message_; }
    // 借用终态分类供宿主观察。
    const std::string& category() const noexcept { return category_; }
  private:
    runtime::DomainId owner_;
    AskReceipt original_;
    events::MailboxId target_;
    std::string category_, body_;
    events::EventId message_{};
    bool acknowledged_ = false;
};
}
