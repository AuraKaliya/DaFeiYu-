#pragma once
#include <dafeiyu/value.hpp>

namespace dfy::sdk {
struct ConversationKey {
    std::string instance, channel_instance, account, external_id;

    // 将实例、渠道实例、账号和外部会话身份序列化。
    Json to_json() const {
        return {{"instance", instance}, {"channel_instance", channel_instance}, {"account", account},
            {"external_id", external_id}};
    }

    // 将完整会话身份编码为稳定且隔离渠道账号的标识。
    std::string id() const { return Json::array({instance, channel_instance, account, external_id}).dump(); }
};

// 这些值仅描述交付绑定；授权资格由核心签发并校验。
struct DeliveryBinding {
    Json channel;
    Json authorization;
    std::string target;
    Json reply_reference;

    // 序列化渠道身份、授权说明、回复目标及引用信息。
    Json to_json() const {
        return {{"channel", channel}, {"authorization", authorization}, {"target", target},
            {"reply_reference", reply_reference}};
    }
};
}
