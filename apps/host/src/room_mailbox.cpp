#include "application.hpp"
#include "run_log.hpp"
#include <nlohmann/json.hpp>

namespace dfy::app {
// 对房间服务的原输入编码并投递，发送方身份取自本体绑定。
// 步骤：核对房间及正文上限 → 选择原分类 → 在独立信箱取得接纳回执。
events::DeliveryReceipt Application::post_room_event(RoomEvent event, const std::string& room, const std::string& text) {
    if (!session_.accepting() || room.empty() || room.size() > 128 || text.empty() || text.size() > 2048)
        throw std::invalid_argument("room: active host and bounded room fact required");
    const char* category;
    switch (event) {
    case RoomEvent::presence_changed: category = "room.presence"; break;
    case RoomEvent::addressed: category = "room.addressed"; break;
    default: throw std::invalid_argument("room: unknown event class");
    }
    const auto data = nlohmann::json{{"room",room},{"text",text}}.dump();
    const auto receipt = mailbox_host_.network->deliver_next(room_sender_,room_mailbox_,
        events::mailbox_text(category,1,data,mailbox_host_.network->payload_limit()));
    if (run_log_ && receipt.state == events::DeliveryState::accepted)
        run_log_->mail("mail.accepted",room_mailbox_.value,room_sender_,receipt.event,category,data);
    return receipt;
}

// 在房间地址上执行同一事件规范下的主动分流。
MailboxIntakeResult Application::review_room() { return mailbox_intake_.review(room_mailbox_); }
}
