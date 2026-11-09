#pragma once
#include "plugins.hpp"
#include "store.hpp"
#include <dafeiyu/conversation.hpp>

namespace dfy {
class Conversations {
    StateStore& store_;
    StateWriter writer_;
    PluginRuntime& plugins_;
    const Profile& profile_;
    Json bind(const std::string& service, const std::string& target, Json authorization, const Json& reply_reference);
public:
    Conversations(StateStore& store, PluginRuntime& plugins, const Profile& profile) : store_(store), writer_(store.claim("conversations", {"conversation", "channel_authority"})), plugins_(plugins), profile_(profile) {}
    void start();
    Json local(const std::string& target, const std::string& service = {});
    Json channel(const std::string& plugin, const std::string& service, const Json& input);
    Json activity(const std::string& id, const std::string& target, const std::string& reply_service = {});
    void validate_identity(const Json& delivery) const;
    void authorize(const Json& delivery);
    Json ensure(const Json& route);
    Json close(const std::string& id);
};
}
