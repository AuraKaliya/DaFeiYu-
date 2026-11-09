#pragma once
#include <dafeiyu/value.hpp>

namespace dfy::sdk {
struct ConversationKey {
    std::string instance, channel_instance, account, external_id;
    Json to_json() const {
        return {{"instance", instance}, {"channel_instance", channel_instance}, {"account", account}, {"external_id", external_id}};
    }
    std::string id() const { return Json::array({instance, channel_instance, account, external_id}).dump(); }
};
// Descriptive records only: the core issues and checks authority, not these values.
struct DeliveryBinding {
    Json channel;
    Json authorization;
    std::string target;
    Json reply_reference;
    Json to_json() const {
        return {{"channel", channel}, {"authorization", authorization}, {"target", target}, {"reply_reference", reply_reference}};
    }
};
}
