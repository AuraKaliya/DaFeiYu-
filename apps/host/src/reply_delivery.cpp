#include "reply_delivery.hpp"
#include "run_log.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 交付一次原结果，暂满和已接纳各自保持独立状态。
// 步骤：识别既有接纳 → 尝试入箱 → 记录稳定的消息身份。
bool ReplyDelivery::advance(events::Mailboxes& network, runtime::DomainId sender, RunLog* log) {
    if (message_.sequence) return false;
    try {
        const auto receipt = network.deliver_next(sender,target_,events::mailbox_text(category_,1,body_,network.payload_limit()));
        message_ = receipt.event;
    } catch (const std::length_error&) { return false; }
    if (log) {
        log->mail("mail.accepted",target_.value,sender,message_,category_,body_);
        log->append("interaction.delivered",{{"ask",original_.ask},{"item",original_.item},{"revision",original_.revision},
            {"eventDomain",original_.origin.domain},{"event",original_.origin.sequence},{"category",category_},
            {"mailbox",target_.value},{"message",message_.sequence},{"body",nlohmann::json::parse(body_)}});
    }
    return true;
}

// 只接纳对应原插件、原事务及实际信箱消息的回执。
// 步骤：验证原身份 → 复用已有收件 → 固定真实收件记录。
bool ReplyDelivery::acknowledge(runtime::DomainId owner, const AskReceipt& receipt, RunLog* log) {
    if (!message_.sequence || owner != owner_ || receipt.origin != original_.origin || receipt.ask != original_.ask ||
        receipt.item != original_.item || receipt.revision != original_.revision ||
        (receipt.message.sequence && receipt.message != message_)) return false;
    if (acknowledged_) return receipt.disposition == original_.disposition;
    original_.disposition = receipt.disposition;
    acknowledged_ = true;
    if (log) log->append("interaction.acknowledged",{{"eventDomain",original_.origin.domain},{"event",original_.origin.sequence},
        {"ask",original_.ask},{"item",original_.item},{"revision",original_.revision},{"disposition",receipt.disposition},
        {"mailbox",target_.value},{"message",message_.sequence},{"legacyReceipt",receipt.message.sequence == 0}});
    return true;
}
}
